#include "edt_spawn_system.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "scn_skinning_prototype_desc.h"
#include "ecs_event.hpp"
#include "ecs_component.h"
#include "texture/rnd_texture_2d_desc.h"

void edt::spawn_system(entt::registry& spawner)
{
	auto anchors = spawner.view<scn::scene_anchor_component>();
	if (!anchors.empty()) return;

	entt::entity world_anchor;
	if (anchors.empty()) {
		world_anchor = spawner.create();
		spawner.emplace<scn::scene_anchor_component>(world_anchor);
		spawner.emplace<scn::name_component>(world_anchor, scn::name_component{ .name = "Anchor" });
		spawner.emplace<scn::depth_level>(world_anchor);
		spawner.emplace<scn::local_transform>(world_anchor, glm::mat4{ 1.0 });
		spawner.emplace<scn::world_transform>(world_anchor, glm::mat4{ 1.0 });
	} else {
		world_anchor = anchors.front();
	}

	auto& children = spawner.get_or_emplace<scn::children_component>(world_anchor).children;

	if (true) {
		rnd::texture_2d_desc txm_desc;
		txm_desc.txm_name = "window";
		//txm_desc.txm_tag = res::tag::make("window.png");
		auto& header = txm_desc.header;
		header.data.format = rnd::driver::texture_header::TYPE::RGBA8;
		header.data.extent.width = 4587;
		header.data.extent.height = 8000;

		json::object txm_js;
		txm_desc.serialize(txm_js);
		std::string str_data = json::serialize(txm_js);
		std::vector<std::byte> txm_data;
		txm_data.resize(str_data.size());
		std::memcpy(txm_data.data(), str_data.data(), str_data.size());
		res::get_system().store(res::tag(res::tag::memory, "window.desc"), txm_data);

		res::tag window_material = res::tag(res::tag::memory, "window_material.desc");
		scn::material_desc mlt;

		rnd::shader_config::constant_data tmpdesc;

		tmpdesc.program = rnd::shader_config::shader_program_data::build()
			.set_vertex_shader(res::tag::make("shaders/mix_opaque_trans_scene.vert"))
			.set_fragment_shader(res::tag::make("shaders/mix_opaque_trans_scene.frag"));

		tmpdesc.defines = { "USE_TXM_AS_DIFFUSE", "LIGHTS_ENABLED" };
		mlt.queue = scn::pass_queue::MIX;
		mlt.cdata = tmpdesc;
		mlt.samplers_textures = { res::tag(res::tag::memory, "window.desc") };

		json::object mlt_js;
		mlt.serialize(mlt_js);
		std::string str_data2 = json::serialize(mlt_js);
		std::vector<std::byte> mlt_data;
		mlt_data.resize(str_data2.size());
		std::memcpy(mlt_data.data(), str_data2.data(), str_data2.size());
		res::get_system().store(window_material, mlt_data);
	}

	{
		scn::material_desc mlt;

		mlt.cdata.program = rnd::shader_config::shader_program_data::build()
			.set_vertex_shader(res::tag::make("shaders/scene.vert"))
			.set_fragment_shader(res::tag::make("shaders/scene.frag"));

		mlt.cdata.defines;
		mlt.queue = scn::pass_queue::OPAQUE;
		mlt.render_mode = rnd::driver::RENDER_MODE::LINE;
		mlt.albedo = ds::color(glm::vec3(0.f), 1.0f);


		//json::object mlt_js;
		//mlt.serialize(mlt_js);
		res::tag web_material = res::tag(res::tag::memory, "editor/web_material.desc");
		res::get_system().pin_resource(web_material, std::move(mlt));
	}

	//  camera
	{
		glm::ivec4 viewport{ glm::zero<glm::ivec2>(), glm::ivec2{1080, 720} };
		glm::vec3 rotation(0);
		rotation.x = -glm::radians(45.0f);
		auto ecs_entity = spawner.create();
		children.push_back((ecs_entity));
		spawner.emplace<scn::name_component>(ecs_entity, "Editor camera");
		spawner.emplace<scn::parent_component>(ecs_entity, world_anchor);
		spawner.emplace<scn::depth_level>(ecs_entity, 0u);
		spawner.emplace<scn::camera_component>(ecs_entity, scn::camera_component{ .viewport = viewport });
		spawner.emplace<scn::local_transform>(ecs_entity);
		spawner.emplace<scn::world_transform>(ecs_entity);
		spawner.emplace<scn::renderable>(ecs_entity);
		spawner.emplace<scn::mouse_controller_component>(ecs_entity, scn::mouse_controller_component{ .rotation = rotation });
		if (spawner.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
			spawner.ctx().get<ecs::event<scn::hierarchy_updated>>().emit((ecs_entity));
		} else {
			ecs::event<scn::hierarchy_updated> event;
			event.emit((ecs_entity));
			spawner.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
		}
		if (spawner.ctx().contains<ecs::event<scn::transform_updated>>()) {
			spawner.ctx().get<ecs::event<scn::transform_updated>>().emit((ecs_entity));
		} else {
			ecs::event<scn::transform_updated> event;
			event.emit(ecs_entity);
			spawner.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
		}
	}


	// web
	if (true) {
		auto web = scn::generate_web({ 50, 50 });
		res::tag web_tag = res::tag(res::tag::memory, "web.desc");
		rnd::geometry_desc geom_desc;
		geom_desc.layout = {
			{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
			{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
			{rnd::driver::SHADER_DATA_TYPE::VEC2_F, "uv"},
		};

		geom_desc.indices = web.indices;
		geom_desc.vertices.resize(web.vertices.size() * sizeof(scn::vertex));
		std::memcpy(geom_desc.vertices.data(), (std::byte*)web.vertices.data(), geom_desc.vertices.size());
		auto geom_tag = web_tag;
		json::object geom_js;
		geom_desc.serialize(geom_js);

		std::string str_data = json::serialize(geom_js);
		std::vector<std::byte> geom_data;
		geom_data.resize(str_data.size());
		std::memcpy(geom_data.data(), str_data.data(), str_data.size());

		res::get_system().store(geom_tag, geom_data);

		scn::prototype_desc web_prototype_desc;
		web_prototype_desc.geometry = res::get_system().require<rnd::geometry_desc>(geom_tag);
		web_prototype_desc.root.name = "Editor Web";
		web_prototype_desc.root.local = glm::scale(glm::vec3(1, 0, 1));
		web_prototype_desc.root.mesh = scn::prototype_desc::mesh_t{
			.vx_begin = 0, .vx_end = web.vertices.size(),
			.ind_begin = 0, .ind_end = web.indices.size(),
			.material = res::get_system().require<scn::material_desc>(res::tag(res::tag::memory, "editor/web_material.desc"))
		};

		web_prototype_desc.load_prototype(spawner, world_anchor);
	}

	if (true) {

		auto geom = scn::generate_cube();
		res::tag cube_tag = res::tag(res::tag::memory, "cube.desc");
		// windows objects
		for (int i = 0; i < 1; ++i) {
			auto wind = spawner.create();
			children.push_back((wind));

			rnd::geometry_desc geom_desc;
			geom_desc.layout = {
				{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
				{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
				{rnd::driver::SHADER_DATA_TYPE::VEC2_F, "texture_position"}
			};

			geom_desc.indices = geom.indices;
			geom_desc.vertices.resize(geom.vertices.size() * sizeof(scn::vertex));
			std::memcpy(geom_desc.vertices.data(), (std::byte*)geom.vertices.data(), geom_desc.vertices.size());
			auto geom_tag = cube_tag;
			json::object geom_js;
			geom_desc.serialize(geom_js);

			std::string str_data = json::serialize(geom_js);
			std::vector<std::byte> geom_data;
			geom_data.resize(str_data.size());
			std::memcpy(geom_data.data(), str_data.data(), str_data.size());

			res::get_system().store(geom_tag, geom_data);

			glm::vec2 rnd_pos = glm::diskRand(1.f);
			scn::prototype_desc window_prototype_desc;
			window_prototype_desc.geometry = res::get_system().require<rnd::geometry_desc>(geom_tag);
			window_prototype_desc.root.name = "Window";
			window_prototype_desc.root.local = glm::translate(glm::mat4{ 1.0 }, glm::vec3(rnd_pos.x, 0, rnd_pos.y));
			window_prototype_desc.root.mesh = scn::prototype_desc::mesh_t{
				.vx_begin = 0, .vx_end = geom.vertices.size(),
				.ind_begin = 0, .ind_end = geom.indices.size(),
				.material = res::get_system().require<scn::material_desc>(res::tag(res::tag::memory, "window_material.desc"))
			};
			window_prototype_desc.root.children = { window_prototype_desc.root };
			window_prototype_desc.root.children[0].name = "Window2";
			window_prototype_desc.root.children[0].local = glm::translate(glm::mat4{ 1.0 }, glm::vec3(rnd_pos.x + 3, 0, rnd_pos.y + 3));

			window_prototype_desc.load_prototype(spawner, world_anchor);
		}
	}

	// light
	{
		auto light = spawner.create();
		children.push_back((light));
		spawner.emplace<scn::directional_light>(light,
													 glm::vec4(-0.2f, -1.0f, -0.3f, 0.0),
													 glm::vec4(0.5f, 0.5f, 0.5f, 1.0),
													 glm::vec4(0.2f, 0.2f, 0.2f, 1.0),
													 glm::vec4(1)
		);

		spawner.emplace<scn::name_component>(light, scn::name_component{ .name = "Global Light" });
		spawner.emplace<scn::parent_component>(light, world_anchor);
		spawner.emplace<scn::depth_level>(light);
		spawner.emplace<scn::renderable>(light);
		spawner.ctx().get<ecs::event<scn::hierarchy_updated>>().emit((light));
	}

	// sky
	{
		auto sky = spawner.create();
		children.push_back(sky);
		spawner.emplace<scn::sky_component>(sky, scn::sky_component{ .cube_map = std::vector<res::tag>{
			res::tag::make("skybox/right.jpg"),
			res::tag::make("skybox/left.jpg"),
			res::tag::make("skybox/bottom.jpg"),
			res::tag::make("skybox/top.jpg"),
			res::tag::make("skybox/front.jpg"),
			res::tag::make("skybox/back.jpg"),
			}
		});
		spawner.emplace<scn::renderable>(sky);
		spawner.emplace<scn::name_component>(sky, scn::name_component{ .name = "Sky" });
		spawner.emplace<scn::parent_component>(sky, world_anchor);
		spawner.emplace<scn::depth_level>(sky);
		spawner.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(sky);
	}
}