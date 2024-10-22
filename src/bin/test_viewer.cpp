#include <stdlib.h>
#include <string.h>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "imgui/imgui.h"
#include <GL/gl.h>
#include <GL/glext.h>

#include "tiny_expr/tinyexpr.h"

#include "P1.h"
#include "chrono.h"
#include "conjugate_gradient.h"
#include "cube.h"
#include "fem_matrix.h"
#include "logging.h"
#include "mesh.h"
#include "mesh_bounds.h"
#include "mesh_gpu.h"
#include "mesh_io.h"
#include "ndc.h"
#include "shaders.h"
#include "sphere.h"
#include "viewer.h"

float bgcolor[4] = { 0.3, 0.3, 0.3, 1.0 };
bool draw_surface = true;
bool draw_edges = false;

bool autoscale = true;
bool started = false;
bool one_step = false;
bool reset = false;
float scale_min;
float scale_max;
int iter_per_frame = 1;

/* RHS expression of the PDE */
char rhs_expression[128] = "x^2 - y^2";
bool rhs_show_error = false;
double rhs_x, rhs_y, rhs_z;
te_variable rhs_vars[3] = { { "x", &rhs_x }, { "y", &rhs_y }, { "z", &rhs_z } };
te_expr *te_rhs = NULL;

struct FEMData;

static void syntax(char *prg_name);
static int load_mesh(Mesh &mesh, int argc, char **argv);
static void rescale_and_recenter_mesh(Mesh &mesh);
static void init_camera_for_mesh(const Mesh &mesh, Camera &camera);
static void update_all(FEMData &fem, Mesh &mesh, GPUMesh &mesh_gpu);
static void draw_scene(const Viewer &viewer, int shader,
		       const GPUMesh &gpu_mesh);
static void draw_gui(FEMData &fem);
static void key_cb(int key, int action, int mods, void *args);

static void add_mass_to_stiffness(FEMatrix &S, const FEMatrix &M);
static void get_attr_bounds(const Mesh &m, float *attr_min, float *attr_max);

struct FEMData {
	FEMData(const Mesh &m);
	const Mesh &m;
	size_t N;
	TArray<double> f;
	TArray<double> u;
	FEMatrix A; // Matrix A of the system Au=Mf
	FEMatrix M; // Mass matrix
	TArray<double> b; // rhs b = Mf of the system
	TArray<double> r; // current residue
	TArray<double> p; // internal for cg
	TArray<double> Ap; // internal for cg
	size_t iterate = 0;
	bool converged = false;
	double relative_error = 0;

	void clear_solution();
	bool construct_rhs();
	size_t do_iterate(size_t max_iter);
	void transfer_rhs_to_mesh(Mesh &m);
	void transfer_sol_to_mesh(Mesh &m);
};

FEMData::FEMData(const Mesh &m)
	: m(m)
	, N(m.vertex_count())
	, f(N)
	, u(N, 0.0)
	, b(N)
	, r(N)
	, p(N)
	, Ap(N)
{
	build_P1_mass_matrix(m, M);
	build_P1_stiffness_matrix(m, A);
	add_mass_to_stiffness(A, M);
}

void FEMData::clear_solution()
{
	for (size_t i = 0; i < N; ++i) {
		u[i] = f[i];
	}
	iterate = 0;
	converged = false;
}

bool FEMData::construct_rhs()
{
	te_expr *test = te_compile(rhs_expression, rhs_vars, 3, NULL);
	if (!test)
		return false;
	te_free(te_rhs);
	te_rhs = test;
	for (size_t i = 0; i < N; ++i) {
		rhs_x = m.positions[i].x;
		rhs_y = m.positions[i].y;
		rhs_z = m.positions[i].z;
		f[i] = te_eval(te_rhs);
	}
	M.mvp(f.data, b.data);
	return true;
}

size_t FEMData::do_iterate(size_t max_iter)
{
	size_t iter = conjugate_gradient_solve(A, b.data, u.data, r.data,
					       p.data, Ap.data, &relative_error,
					       1e-6, max_iter, iterate != 0);
	iterate += iter;
	if (iter < max_iter) {
		converged = true;
	}
	return iter;
}

void FEMData::transfer_rhs_to_mesh(Mesh &m)
{
	m.attr.resize(m.vertex_count());
	for (size_t i = 0; i < m.vertex_count(); ++i) {
		m.attr[i] = f[i];
	}
}

void FEMData::transfer_sol_to_mesh(Mesh &m)
{
	m.attr.resize(m.vertex_count());
	for (size_t i = 0; i < m.vertex_count(); ++i) {
		m.attr[i] = u[i];
	}
}

int main(int argc, char **argv)
{
	log_init(0);

	/* Load Mesh */
	Mesh mesh;
	if (load_mesh(mesh, argc, argv)) {
		syntax(argv[0]);
		exit(EXIT_FAILURE);
	}
	LOG_MSG("Loaded mesh.");
	rescale_and_recenter_mesh(mesh);
	LOG_MSG("Mesh rescaled and recentered.");

	/* Prepare FEM data */
	FEMData fem(mesh);
	fem.construct_rhs();
	fem.transfer_sol_to_mesh(mesh);
	LOG_MSG("Prepared FEM data.");

	/* Get an OpenGL context through a viewer app. */
	Viewer viewer;
	init_camera_for_mesh(mesh, viewer.camera);
	viewer.init("Viewer App");
	viewer.register_key_callback({ key_cb, NULL });
	LOG_MSG("Viewer initialized.");

	/* Prepare GPU data */
	const char *vert_shader = "./shaders/fem.vert";
	const char *frag_shader = "./shaders/fem.frag";
	int shader = create_shader(vert_shader, frag_shader);
	if (!shader) {
		exit(EXIT_FAILURE);
	}
	LOG_MSG("Shader initialized.");
	GPUMesh gpu_mesh;
	gpu_mesh.m = &mesh;
	fem.transfer_rhs_to_mesh(mesh);
	get_attr_bounds(mesh, &scale_min, &scale_max);
	gpu_mesh.upload();

	/* Main Loop */
	while (!viewer.should_close()) {
		viewer.poll_events();
		update_all(fem, mesh, gpu_mesh);
		viewer.begin_frame();
		draw_scene(viewer, shader, gpu_mesh);
		draw_gui(fem);
		viewer.end_frame();
	}

	viewer.fini();

	return (EXIT_SUCCESS);
}

static void syntax(char *prg_name)
{
	printf("Syntax : %s ($(obj_filename)| cube | sphere) [n]", prg_name);
	printf("         Subdivision number n must be provided in case of "
	       "         cube or sphere mesh.\n");
}

static int load_mesh(Mesh &mesh, int argc, char **argv)
{
	int res = -1;
	if (argc > 2 && strncmp(argv[1], "cube", 4) == 0) {
		res = load_cube(mesh, atoi(argv[2]));
	} else if (argc > 2 && strncmp(argv[1], "sphere", 5) == 0) {
		res = load_sphere(mesh, atoi(argv[2]));
	} else if (argc > 1) {
		res = load_obj(argv[1], mesh);
	}
	return res;
}

static void rescale_and_recenter_mesh(Mesh &mesh)
{
	Aabb bbox = compute_mesh_bounds(mesh);
	Vec3 model_center = (bbox.min + bbox.max) * 0.5f;
	Vec3 model_extent = (bbox.max - bbox.min);
	float model_size = max(model_extent);
	if (model_size == 0) {
		printf("Warning : Mesh is empty or reduced to a point.\n");
		model_size = 1;
	}
	for (size_t i = 0; i < mesh.vertex_count(); ++i) {
		mesh.positions[i] -= model_center;
		mesh.positions[i] /= model_size;
	}
}

static void init_camera_for_mesh(const Mesh &mesh, Camera &camera)
{
	Aabb bbox = compute_mesh_bounds(mesh);
	Vec3 model_center = (bbox.min + bbox.max) * 0.5f;
	Vec3 model_extent = (bbox.max - bbox.min);
	float model_size = max(model_extent);
	if (model_size == 0) {
		printf("Warning : Mesh is empty or reduced to a point.\n");
		model_size = 1;
	}
	camera.set_target(model_center);
	Vec3 start_pos = (model_center + 2.f * Vec3(0, 0, model_size));
	camera.set_position(start_pos);
	camera.set_near(0.01 * model_size);
	camera.set_far(100 * model_size);
}

static void add_mass_to_stiffness(FEMatrix &S, const FEMatrix &M)
{
	const Mesh *m = S.m;

	for (size_t i = 0; i < m->vertex_count(); ++i) {
		S.diag[i] += M.diag[i];
	}
	for (size_t i = 0; i < m->triangle_count(); ++i) {
		S.off_diag[3 * i + 0] += M.off_diag[i];
		S.off_diag[3 * i + 1] += M.off_diag[i];
		S.off_diag[3 * i + 2] += M.off_diag[i];
	}
}

static void get_attr_bounds(const Mesh &m, float *attr_min, float *attr_max)
{
	if (!m.vertex_count())
		return;
	float min = m.attr[0];
	float max = min;
	for (size_t i = 1; i < m.vertex_count(); ++i) {
		if (m.attr[i] < min) {
			min = m.attr[i];
		} else if (m.attr[i] > max) {
			max = m.attr[i];
		}
	}
	*attr_min = min;
	*attr_max = max;
}

static void update_all(FEMData &fem, Mesh &mesh, GPUMesh &gpu_mesh)
{
	bool needs_upload = true;
	if (started || one_step) {
		fem.do_iterate(iter_per_frame);
		if (one_step) {
			one_step = false;
		}
		fem.transfer_sol_to_mesh(mesh);
		if (autoscale) {
			get_attr_bounds(mesh, &scale_min, &scale_max);
		}
	} else if (reset) {
		fem.clear_solution();
		fem.transfer_rhs_to_mesh(mesh);
		if (autoscale) {
			get_attr_bounds(mesh, &scale_min, &scale_max);
		}
		reset = false;
	} else {
		needs_upload = false;
	}
	if (needs_upload) {
		gpu_mesh.update_attr();
	}
	if (fem.converged) {
		started = false;
	}
}

static void draw_scene(const Viewer &viewer, int shader,
		       const GPUMesh &gpu_mesh)
{
	glClearColor(bgcolor[0], bgcolor[1], bgcolor[2], bgcolor[3]);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	const Camera &camera = viewer.camera;
	glUseProgram(shader);
	Mat4 proj = camera.view_to_clip();
	Mat4 vm = camera.world_to_view();
	Vec3 camera_pos = camera.get_position();
	glUniformMatrix4fv(0, 1, 0, &vm(0, 0));
	glUniformMatrix4fv(1, 1, 0, &proj(0, 0));
	glUniform3fv(2, 1, &camera_pos[0]);
	glUniform1f(4, scale_min);
	glUniform1f(5, scale_max);

	if (draw_surface) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		float offset = reversed_z ? -1.f : 1.f;
		glPolygonOffset(offset, offset);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glUniform1i(3, true);
		gpu_mesh.draw();
	}
	if (draw_edges) {
		glDisable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.f, 0.f);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glUniform1i(3, false);
		gpu_mesh.draw();
	}
}

static void draw_gui(FEMData &fem)
{
	ImGui::Begin("Controls");
	ImGui::Text("Solves -\\Delta u + u = f");
	ImGui::Text("------------------------");

	ImGui::Text("Enter math expression for f below:");
	ImGui::InputText("", rhs_expression, IM_ARRAYSIZE(rhs_expression));
	ImGui::SameLine();
	if (ImGui::Button("Apply")) {
		if (!fem.construct_rhs()) {
			rhs_show_error = true;
		}
	}
	if (rhs_show_error) {
		ImGui::Begin("Error");
		if (ImGui::Button("ok")) {
			//rhs_show_error = false;
		}
		ImGui::End();
	}

	ImGui::Checkbox("Autoscale", &autoscale);
	ImGui::DragInt("Iterations per step", &iter_per_frame, 1, 1, 10);
	if (ImGui::Button("Start")) {
		started = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Stop")) {
		started = false;
	}
	ImGui::SameLine();
	if (ImGui::Button("One step")) {
		if (!started) {
			one_step = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset")) {
		reset = true;
	}
	ImGui::Text("Iterate : %zu", fem.iterate);
	ImGui::Text("Relative error : %g", fem.relative_error);
	ImGui::Text("Number of DOF : %zu", fem.N);

	float fps = ImGui::GetIO().Framerate;
	ImGui::Text("Average %.3f ms/frame (%.1f FPS)", 1000.0f / fps, fps);
	ImGui::End();
}

static void key_cb(int key, int action, int mods, void *args)
{
	(void)mods;
	(void)args;
	if (key == GLFW_KEY_S && action == GLFW_PRESS) {
		draw_surface = !draw_surface;
		return;
	}
	if (key == GLFW_KEY_E && action == GLFW_PRESS) {
		draw_edges = !draw_edges;
		return;
	}
}

