#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_material_preview.h"
#include "edt_material_window_model.h"
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_model.h>
#include <boost/json.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The material window's live preview (#1196 PR 2): its preview set and camera, the memory copy of the material the shape
// draws with, the shape and sky choices, and what is given up on a layer change and on close. No pixels.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr const char* k_preview_template = R"({
	"__type": "world_desc", "__version": 5, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": [
		{ "__id": "faa1152ccb1457e8", "name": "Skybox",
			"components": { "skybox_desc": { "__type": "skybox_desc", "__version": 1 } } }
	]
})";

constexpr const char* k_plain_material = R"({"__type":"material_desc","__version":2,"queue":"opaque"})";
constexpr const char* k_sky_material = R"({"__type":"material_desc","__version":2,"shader":"res://shaders/sky.shader.desc"})";

struct material_preview_fixture : test_util::scene_env
{
	material_preview_fixture()
		: test_util::scene_env(test_util::test_descs::without)
	{
		fs::create_directories(res_root);
		fs::create_directories(lower / "shapes");
		// The unit shapes the preview draws, in the lower layer as the engine's are.
		for (const char* name : { "sphere", "box", "plane" }) {
			for (const char* suffix : { ".geom.desc", ".geom.bin" }) {
				const std::string file = std::string(name) + suffix;
				fs::copy_file(fs::path(SNK_CORE_RES_DIR) / "shapes" / file, lower / "shapes" / file, fs::copy_options::overwrite_existing);
			}
		}
		// The engine's sky shader, as core/res has it (the stages are not compiled here).
		fs::create_directories(lower / "shaders");
		write(lower, "shaders/sky.vert", "#version 450\n");
		write(lower, "shaders/sky.frag", "#version 450\n");
		write(lower, "shaders/sky.shader.desc",
			R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/sky.vert","fragment":"res://shaders/sky.frag"})");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		engine::render::register_render_descs(*descs);
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(k_preview_template));
		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
	}

	~material_preview_fixture()
	{
		preview.reset();
		model.reset();
		document.reset();
		mgr.reset();
		hot.reset();
		engine::render::unregister_render_descs(*descs);
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	static void write(const fs::path& root, std::string_view name, std::string_view content)
	{
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	res::tag put(std::string_view name, std::string_view content) const
	{
		write(res_root, name, content);
		return res::tag{ "res://" + std::string(name) };
	}

	// Opens `content` as a material document with its model and its preview, the preview updated once (one frame shown).
	edt::material_preview& open(std::string_view content)
	{
		document.emplace(services, layout());
		const edt::open_result opened = document->open(put("lit.desc", content));
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		model.emplace(*document, services);
		preview.emplace(*document, services);
		preview->set_targets_release([this](const std::vector<res::tag>& tags) { released.insert(released.end(), tags.begin(), tags.end()); });
		preview->update(model->shader());
		return *preview;
	}

	// The material copy as it is in memory (null: there is none).
	std::optional<json::object> stored_material() const
	{
		const auto found = ram->files.find(preview->material_tag());
		if (found == ram->files.end()) return std::nullopt;
		const std::string text(reinterpret_cast<const char*>(found->second.data()), found->second.size());
		return json::parse(text).as_object();
	}

	std::string geometry_shown()
	{
		entt::registry* reg = preview->registry();
		const entt::entity shape = preview->shape_entity();
		if (!reg || shape == entt::null) return {};
		const auto* geometry = reg->try_get<scn::geometry_component>(shape);
		return geometry ? std::string(geometry->geom_tag.view()) : std::string{};
	}

	// The sky entities of the preview and how many of them are drawn.
	std::pair<std::size_t, std::size_t> skies()
	{
		entt::registry* reg = preview->registry();
		if (!reg) return { 0u, 0u };
		std::size_t all = 0;
		std::size_t shown = 0;
		for (const entt::entity entity : reg->view<scn::sky_component>()) {
			++all;
			if (scn::is_visible(*reg, entity)) ++shown;
		}
		return { all, shown };
	}

	test_util::temp_dir dir{ "edt_material_preview" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	ecs::system_factory sf;
	edt::editor_services services;
	std::shared_ptr<scn::hot_reload_manager> hot;
	std::shared_ptr<scn::context_manager> mgr;
	std::optional<edt::data_document> document;
	std::optional<edt::material_window_model> model;
	std::optional<edt::material_preview> preview;
	std::vector<res::tag> released;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMaterialPreviewTests, material_preview_fixture)

BOOST_AUTO_TEST_CASE(Open_MakesThePreviewSet_AndAnOrbitCameraWithItsTargets)
{
	edt::material_preview& shown = open(k_plain_material);

	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(!shown.sky_material());
	BOOST_REQUIRE(!shown.set_name().empty());
	BOOST_TEST(mgr->find_set(shown.set_name()) != nullptr);
	BOOST_TEST(shown.world_template().view().starts_with("memory://editor/__material_preview/"));
	scn::camera_component* camera = shown.camera();
	BOOST_REQUIRE(camera != nullptr);
	BOOST_TEST(shown.render_target().starts_with("memory://__material_camera_rt/"));
	BOOST_TEST(camera->color_targets.front().view() == shown.render_target());
	BOOST_TEST(camera->depth_target.is_valid());
	BOOST_TEST(camera->tp_accum_target.is_valid());
	BOOST_TEST(camera->tp_reveal_target.is_valid());
	entt::registry& reg = *shown.registry();
	std::size_t orbit = 0;
	for (const entt::entity entity : reg.view<scn::camera_component, scn::mouse_controller_component>()) {
		if (&reg.get<scn::camera_component>(entity) == camera) ++orbit;
	}
	BOOST_TEST(orbit == 1u);

	// Parked (the window is hidden): the camera draws nothing.
	camera->m_viewport.size = { 64, 64 };
	shown.park();
	BOOST_TEST(shown.camera()->m_viewport.size.x == 0);
}

BOOST_AUTO_TEST_CASE(AnEdit_RewritesTheMemoryMaterial_WhichTheShapeDrawsWith)
{
	edt::material_preview& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(shown.material_tag().view().starts_with("memory://editor/material/"));
	const std::optional<json::object> before = stored_material();
	BOOST_REQUIRE(before.has_value());
	BOOST_TEST(before->at("__type").as_string() == "material_desc");
	BOOST_TEST(!before->contains("__parent"));

	const json::array albedo{ 0.5, 0.25, 0.125, 1.0 };
	BOOST_REQUIRE(document->edit([&](json::value& value) {
		json::value& uniforms = value.as_object()["uniforms"];
		if (!uniforms.is_object()) uniforms = json::object{};
		uniforms.as_object()["albedo"] = albedo;
		return true;
	}));
	BOOST_TEST(document->is_dirty());
	shown.update(model->shader());

	const std::optional<json::object> after = stored_material();
	BOOST_REQUIRE(after.has_value());
	BOOST_TEST((after->at("uniforms").as_object().at("albedo") == json::value(albedo)));
	entt::registry& reg = *shown.registry();
	const entt::entity shape = shown.shape_entity();
	BOOST_REQUIRE(shape != entt::null);
	const auto* material = reg.try_get<scn::material_desc_component>(shape);
	BOOST_REQUIRE(material != nullptr);
	BOOST_TEST(material->mlt_desc.get_tag().view() == shown.material_tag().view());
	// The file is not written by the preview.
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(TheShapeChoice_SwapsTheGeometry)
{
	edt::material_preview& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST((shown.shape() == edt::preview_shape::sphere));
	BOOST_TEST(geometry_shown() == "res://shapes/sphere.geom.desc");

	shown.set_shape(edt::preview_shape::cube);
	BOOST_TEST(geometry_shown() == "res://shapes/box.geom.desc");
	shown.set_shape(edt::preview_shape::plane);
	BOOST_TEST(geometry_shown() == "res://shapes/plane.geom.desc");
	const entt::entity shape = shown.shape_entity();
	BOOST_REQUIRE(shape != entt::null);
	BOOST_TEST(shown.registry()->get<scn::material_desc_component>(shape).mlt_desc.get_tag().view() == shown.material_tag().view());
}

BOOST_AUTO_TEST_CASE(TheSkyToggle_HidesTheTemplatesSky_AndShowsItAgain)
{
	edt::material_preview& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(shown.sky());
	BOOST_TEST((skies() == std::pair<std::size_t, std::size_t>{ 1u, 1u }));

	shown.set_sky(false);
	BOOST_TEST((skies() == std::pair<std::size_t, std::size_t>{ 1u, 0u }));
	shown.set_sky(true);
	BOOST_TEST((skies() == std::pair<std::size_t, std::size_t>{ 1u, 1u }));

	// A world made again keeps the choice.
	shown.set_sky(false);
	shown.release_world();
	shown.rebuild_world();
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST((skies() == std::pair<std::size_t, std::size_t>{ 1u, 0u }));
}

BOOST_AUTO_TEST_CASE(ALayerChange_DropsTheSetAndMakesItAgain_AndCloseGivesEverythingUp)
{
	edt::material_preview& shown = open(k_plain_material);
	BOOST_REQUIRE(shown.has_world());
	const std::string set = shown.set_name();
	const std::string target = shown.render_target();
	const res::tag world = shown.world_template();
	const res::tag material = shown.material_tag();

	shown.release_world();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(mgr->find_set(set) == nullptr);
	BOOST_TEST(shown.registry() == nullptr);
	BOOST_TEST(released.empty());
	// Hidden frames between the two halves of the change make nothing in the old layer.
	shown.update(model->shader());
	BOOST_TEST(!shown.has_world());

	shown.rebuild_world();
	BOOST_REQUIRE(shown.has_world());
	BOOST_TEST(mgr->find_set(set) != nullptr);
	BOOST_TEST(shown.camera() != nullptr);
	BOOST_TEST(shown.shape_entity() != entt::null);

	shown.close();
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(mgr->find_set(set) == nullptr);
	BOOST_REQUIRE(released.size() == 4u);
	BOOST_TEST(released.front().view() == target);
	BOOST_TEST(!ram->files.contains(world));
	BOOST_TEST(!ram->files.contains(material));
}

BOOST_AUTO_TEST_CASE(ASkyMaterial_HasNoPreview)
{
	edt::material_preview& shown = open(k_sky_material);

	BOOST_TEST_MESSAGE(model->shader().error);
	BOOST_TEST(model->shader().vertex.view() == "res://shaders/sky.vert");
	BOOST_TEST(edt::is_sky_material(model->shader()));
	BOOST_TEST(shown.sky_material());
	BOOST_TEST(!shown.has_world());
	BOOST_TEST(shown.registry() == nullptr);
	BOOST_TEST(!ram->files.contains(shown.material_tag()));
	BOOST_TEST(released.empty());
	BOOST_TEST(shown.set_name().empty());
}

BOOST_AUTO_TEST_SUITE_END()
