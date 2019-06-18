#include "hw_local.hh"
using namespace howler;

#include <set>

void instance::begin_frame() {
	m_frame = make_q3frame();
	m_frame->scenes.emplace_back();
}

static constexpr qm::mat4_t ui_ortho = qm::mat4_t::ortho(0, 480, 0, 640, 0, 1);

template <typename T>
constexpr int8_t compare3(T const & A, T const & B) {
	if (A > B) return 1;
	if (A < B) return -1;
	return 0;
}

struct q3drawmesh {
	q3drawmesh() = delete;
	q3drawmesh(q3mesh_ptr const & mesh, qm::mat4_t const & mvp) : mesh(mesh), mvp(mvp) {}
	q3drawmesh(q3drawmesh const &) = delete;
	q3drawmesh(q3drawmesh &&) = default;
	
	q3drawmesh & operator = (q3drawmesh const &) = delete;
	q3drawmesh & operator = (q3drawmesh &&) = default;
	
	q3mesh_ptr mesh;
	qm::mat4_t mvp;
};

struct q3drawset {
	q3shader_ptr shader;
	std::vector<q3drawmesh> meshes;
	
	q3drawset() = delete;
	q3drawset(q3shader_ptr const & shader, std::vector<q3drawmesh> & meshes) : shader(shader), meshes(std::move(meshes)) {}
	q3drawset(q3drawset const &) = delete;
	q3drawset(q3drawset &&) = default;
	
	q3drawset & operator = (q3drawset const &) = delete;
	q3drawset & operator = (q3drawset &&) = default;
	
	static inline bool compare(q3drawset & A, q3drawset & B) {	
		
		switch (compare3(A.shader->sort, B.shader->sort)) {
			case -1: return true;
			case 1: return false;
			case 0: break;
		}
		
		switch (compare3(A.shader->depthwrite, B.shader->depthwrite)) {
			case -1: return false;
			case 1: return true;
			case 0: break;
		}
		
		return A.shader.get() < B.shader.get();
	}
};

void instance::end_frame(float time) {
	
	q3frame_ptr drawframe = m_frame;
	m_frame.reset();
	
	gl::depth_write(true);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	
	m_ui_draw = false;
	m_shader_color = {1, 1, 1, 1};
	
	std::vector<q3drawmesh> debug_meshes;
	bool debug_enabled = r_showtris->integer || r_showedges->integer;
	
	for (q3scene const & scene : drawframe->scenes) {
		
		if (!scene.finalized) continue;
		
		qm::mat4_t p = qm::mat4_t::perspective(qm::deg2rad(scene.ref.fov_y), scene.ref.width, scene.ref.height, 4, m_cull * 2);
		qm::mat4_t v = qm::mat4_t::translate(-scene.ref.vieworg[1], scene.ref.vieworg[2], -scene.ref.vieworg[0]);
		qm::quat_t rq = qm::quat_t::identity();
		rq *= qm::quat_t { {1, 0, 0}, qm::deg2rad(scene.ref.viewangles[PITCH]) };
		rq *= qm::quat_t { {0, 0, 1}, qm::deg2rad(scene.ref.viewangles[ROLL]) + qm::pi };
		rq *= qm::quat_t { {0, 1, 0}, qm::deg2rad(scene.ref.viewangles[YAW]) };
		qm::mat4_t r = qm::mat4_t { rq };
		qm::mat4_t vp = (v * r) * p;
		
		qm::mat4_t sbvp = r * qm::mat4_t::perspective(qm::deg2rad(scene.ref.fov_y), scene.ref.width, scene.ref.height, 0.125, 8);
		
		std::unordered_map<q3shader_ptr, std::vector<q3drawmesh>> draw_map;
		
		if (!(scene.ref.rdflags & RDF_NOWORLDMODEL) && m_world) {
			qm::mat4_t m = qm::mat4_t::scale(1, -1, 1);
			for (auto const & dmesh : m_world->get_vis_model(scene.ref)->meshes) {
				qm::mat4_t mvp = m * vp;
				if (debug_enabled) debug_meshes.emplace_back(dmesh.second, mvp);
				if (!dmesh.second) continue;
				draw_map[dmesh.first].emplace_back(dmesh.second, mvp);
			}
		}
		
		for (auto const & cmd : scene.c3ds) {
			std::visit( lambda_visit {
				[&](cmd3d::basic_object const & obj) {
					if (!obj.basemodel->model) {
						if (obj.basemodel->base.type == MOD_MESH)
							Com_Error(ERR_FATAL, "instance::end_frame: tried to draw a mesh not setup for rendering");
						else return;
					}
					qm::mat4_t mvp = obj.model_matrix * vp;
					for (auto const & mesh : obj.basemodel->model->meshes) {
						if (debug_enabled) debug_meshes.emplace_back(mesh.second, mvp);
						draw_map[mesh.first].emplace_back(mesh.second, mvp);
					}
				}
			}, cmd);
		}
		
		#ifdef _DEBUG
		if (r_drawcalls->integer) {
			size_t draws = 0;
			for (auto & [shader, meshes] : draw_map) {
				draws += meshes.size();
			}
			Com_Printf("draw_map size: %zu shaders with %zu draws\n", draw_map.size(), draws);
		}
		#endif
		
		std::vector<std::pair<q3shader_ptr, std::vector<q3drawmesh>>> skyboxes;
		
		std::vector<q3drawset> draw_set;
		for (auto & [shader, meshes] : draw_map) {
			if (!shader) continue;
			if (shader->sky_parms) {
				skyboxes.emplace_back(shader, std::move(meshes));
				continue;
			}
			draw_set.emplace_back(
				shader,
				meshes
			);
		}
		
		std::sort(draw_set.begin(), draw_set.end(), q3drawset::compare);
		
		// ================================
		// SKYBOXES
		// ================================
		
		gl::initialize();
		gl::polygon_mode(GL_FRONT_AND_BACK, GL_FILL);
		gl::stencil_test(true);
		gl::depth_test(false);
		gl::cull(false);
		glClear(GL_STENCIL_BUFFER_BIT);
		
		q3skyboxstencilprog->bind();
		
		// stencil skybox zones
		GLuint stencil_id = 1;
		for (auto const & sbp : skyboxes) {
			gl::stencil_func(GL_ALWAYS, stencil_id);
			gl::stencil_op(GL_REPLACE, GL_REPLACE, GL_REPLACE);
			for (auto const & dmesh : sbp.second) {
				q3skyboxstencilprog->mvp(dmesh.mvp);
				dmesh.mesh->draw();
			}
			stencil_id ++;
		}
		
		main_sampler->wrap(GL_CLAMP_TO_EDGE);
		q3skyboxprog->bind();
		q3skyboxprog->mvp(sbvp);
		
		for (auto i = 0; i < 6; i++)
			main_sampler->bind(BINDING_SKYBOX + i);
		
		// draw skyboxes on correct stencil
		stencil_id = 1;
		for (auto const & sbp : skyboxes) {
			gl::stencil_func(GL_EQUAL, stencil_id);
			gl::stencil_op(GL_KEEP, GL_KEEP, GL_KEEP);
			for (auto i = 0; i < 6; i++)
				sbp.first->sky_parms->sides[i]->bind(BINDING_SKYBOX + i);
			skybox->draw();
			stencil_id ++;
		}
		
		// ================================
		// REGULAR GEOMETRY
		// ================================
		
		gl::initialize();
		gl::polygon_mode(GL_FRONT_AND_BACK, GL_FILL);
		gl::depth_test(true);
		gl::stencil_test(true);
		glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		
		if (m_world) m_world->m_lightmap->bind(BINDING_LIGHTMAP);
		
		for (q3drawset const & draw : draw_set) {
			draw.shader->setup_draw();
			for (auto const & stg : draw.shader->stages)
				for (auto const & mesh : draw.meshes) {
					if (!mesh.mesh) continue;
					stg.setup_draw(time, mesh.mvp, qm::mat3_t::identity(), mesh.mesh->uniform_info());
					mesh.mesh->draw();
			}
		}
		
		// ================================
	}
	
	gl::initialize();
	gl::polygon_mode(GL_FRONT_AND_BACK, GL_FILL);
	gl::depth_test(false);
	gl::blend(true);
	gl::cull(false);
	
	main_sampler->bind(BINDING_DIFFUSE);
	
	m_ui_draw = true;
	m_shader_color = {1, 1, 1, 1};
	
	for (auto & c2d : drawframe->c2ds) {
		std::visit(lambda_visit{
			[&](cmd2d::stretchpic const & cmd){
				if (!cmd.shader) return;
				
				qm::mat4_t m = qm::mat4_t::scale(cmd.w ? cmd.w : cmd.h * (640.0f / 480.0f), cmd.h, 1);
				m *= qm::mat4_t::translate(cmd.x, cmd.y, 0);
				
				qm::mat3_t uv = qm::mat3_t::scale(cmd.s2 - cmd.s1, cmd.t2 - cmd.t1);
				uv *= qm::mat3_t::translate(cmd.s1, cmd.t1);
				
				m_shader_color = cmd.color;
				qm::mat4_t mvp = m * ui_ortho;
				if (debug_enabled) debug_meshes.emplace_back(unitquad, mvp);
				for (auto const & stg : cmd.shader->stages) {
					stg.setup_draw(time, mvp, uv);
					unitquad->draw();
				}
			}
		}, c2d);
	}
	
	//================================================================
	// CUSTOM SHADER PROGRAMS
	//================================================================
	if (debug_enabled) {
		std::sort(debug_meshes.begin(), debug_meshes.end(), [&](q3drawmesh const & A, q3drawmesh const & B) -> bool{ return A.mesh < B.mesh; });
		if (r_showtris->integer || r_showedges->integer) {
			gl::initialize();
			gl::polygon_mode(GL_FRONT_AND_BACK, GL_LINE);
			gl::line_width(r_showtris->value);
			gl::blend(true);
			gl::blend(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
			
			if (r_showtris->integer) {
				gl::stencil_test(true);
				gl::stencil_mask(1);
				gl::stencil_func(GL_EQUAL, 0);
				gl::stencil_op(GL_KEEP, GL_KEEP, GL_INCR);
			}
			
			glClear(GL_STENCIL_BUFFER_BIT);
			
			q3lineprog->bind();
			
			for (q3drawmesh const & d : debug_meshes) {
				q3lineprog->mvp(d.mvp);
				d.mesh->draw();
			}
		}
	}
	//================================================================
	
	ri.WIN_Present(&window);
	
	#ifdef _DEBUG
	if (r_drawcalls->integer)
		Com_Printf("Draw Calls: %zu\n", q3mesh::m_debug_draw_count);
	q3mesh::m_debug_draw_count = 0;
	#endif
}
