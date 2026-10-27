#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/scn_model.h>
#include "edt_level_controller.h"
#include "edt_prefab_document.h"
#include "edt_prefab_editor_panel.h"
#include "null_engine_fixture.hpp"
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_backend_interface.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_router.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <imgui.h>
#include <imgui_internal.h>
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include <chrono>
#include <cstddef>
#include <optional>
#include <memory>
#include <string>

// The prefab preview (#112): opening a prefab creates the set "preview:<tag>", closing destroys it,
// and the level (its contexts, its save, its reload) never sees it.

namespace {

constexpr const char* LEVEL_TWO_WORLDS = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[]},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

// Same name as the preview context: it must not collide.
constexpr const char* LEVEL_WITH_PREVIEW_NAME = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"__prefab_preview","systems":[]}]})";

constexpr const char* LEVEL_ONE_WORLD = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"only","systems":[]}]})";

constexpr const char* PREVIEW_TEMPLATE = R"({
	"__type": "world_desc", "__version": 3, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": [
		{ "__id": "09705f78a49797f0", "name": "PrefabLight",
		  "components": { "directional_light_desc": { "__type": "directional_light_desc", "__version": 1 } } }
	]
})";

constexpr const char* PREFAB = R"({
	"__type": "prefab_desc", "__version": 2,
	"components": { "stat": { "__type": "stat_desc", "hp": 10 } },
	"children": [
		{ "__id": "00000000000000a1", "name": "arm", "components": { "stat": { "__type": "stat_desc", "hp": 3 } } }
	]
})";

struct preview_fixture : test_util::scene_env
{
	preview_fixture()
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		auto& res_ram = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
		res_ram.store(prefab_a, test_util::to_bytes(PREFAB));
		res_ram.store(prefab_b, test_util::to_bytes(PREFAB));
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(PREVIEW_TEMPLATE));

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.assembler = assembler.get();
		services.sfactory = &sf;
		editor = std::make_unique<edt::prefab_document>(services, *rs, *descs);
	}

	std::size_t preview_sets() const
	{
		std::size_t count = 0;
		mgr->for_each_set([&count](scn::context_set& set) {
			if (set.name().starts_with(scn::context_manager::preview_set_prefix)) ++count;
		});
		return count;
	}

	void pump_reload(const res::tag& level_tag, const char* text)
	{
		put("lv.desc", text);
		rs->signal_changed(level_tag);
		pump();
		mgr->update(std::chrono::duration<float>(0.001f));
	}

	const res::tag prefab_a = res::tag::make("prefabs/a.prefab.desc");
	const res::tag prefab_b = res::tag::make("prefabs/b.prefab.desc");
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::scene_document state;
	std::unique_ptr<edt::prefab_document> editor;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPrefabPreviewTests, preview_fixture)

BOOST_AUTO_TEST_CASE(PreviewDrag_KeepsWindowStillAndRoutesOnlyToPreview_TitleDragMovesWindow)
{
	struct imgui_context
	{
		imgui_context()
		{
			ctx = ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(1200.f, 900.f);
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			io.ConfigWindowsMoveFromTitleBarOnly = false;
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		}
		~imgui_context() { ImGui::DestroyContext(ctx); }
		ImGuiContext* ctx = nullptr;
	} imgui;
	struct stub_backend : gui::imgui_backend_interface
	{
		using imgui_backend_interface::imgui_backend_interface;
		void init() override {}
		void new_frame() override {}
		void render() override {}
		void shutdown() override {}
	};
	struct observed_panel : edt::prefab_editor_panel
	{
		using prefab_editor_panel::prefab_editor_panel;
		void on_render() override
		{
			prefab_editor_panel::on_render();
			image_min = ImGui::GetItemRectMin();
			image_max = ImGui::GetItemRectMax();
		}
		ImVec2 image_min, image_max;
	};

	editor->open(prefab_a);
	BOOST_REQUIRE(editor->is_open());
	test_util::null_engine_fixture rendering(false);
	auto& renderer = rendering.storage.require<rnd::render_system>();
	stub_backend backend(*renderer.get_driver());
	gui::gui_system gui(&backend, nullptr);
	rnd::driver::texture_header header;
	header.data.extent.width = 4;
	header.data.extent.height = 4;
	renderer.get_texture_manager().generate(res::tag{ edt::prefab_document::RT_COLOR }, header);
	auto source = std::make_shared<inp::ecs_input_manager>();
	inp::input_router router(source);
	router.set_default_target(std::nullopt);
	observed_panel panel(renderer, gui);
	panel.set_context(editor.get());
	panel.set_input_router(&router);
	panel.set_open(true);
	auto frame = [&] {
		router.clear_bindings();
		ImGui::NewFrame();
		panel.render();
		ImGui::EndFrame();
	};
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(80.f, 80.f));
	ImGui::SetNextWindowSize(ImVec2(600.f, 450.f));
	ImGui::SetNextWindowDockID(0);
	panel.render();
	ImGui::EndFrame();
	frame();
	auto* window = ImGui::FindWindowByName("Prefab Editor");
	BOOST_REQUIRE(window);
	BOOST_REQUIRE(!window->DockIsActive);
	const ImVec2 initial = window->Pos;
	BOOST_REQUIRE(panel.image_max.x > panel.image_min.x);
	BOOST_REQUIRE(panel.image_max.y > panel.image_min.y);
	const ImVec2 image_center((panel.image_min.x + panel.image_max.x) * 0.5f,
		(panel.image_min.y + panel.image_max.y) * 0.5f);
	auto move = [&](ImVec2 pos) {
		ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
		inp::input_event event;
		event.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ { pos.x, pos.y } });
		source->on_handle_event(nullptr, event);
		frame();
	};
	auto click = [&](bool down) {
		ImGui::GetIO().AddMouseButtonEvent(0, down);
		inp::input_event event;
		event.construct_payload<core::inp::mouse_click_event>(core::inp::mouse_click_event{
			core::inp::MOUSE_BUTTONS::LEFT, down ? core::inp::KEY_ACTION::DOWN : core::inp::KEY_ACTION::UP });
		source->on_handle_event(nullptr, event);
		frame();
	};
	move(image_center);
	click(true);
	move(ImVec2(image_center.x + 60.f, image_center.y + 40.f));
	frame();
	BOOST_TEST(window->Pos.x == initial.x);
	BOOST_TEST(window->Pos.y == initial.y);
	const auto preview = router.route(editor->set_name(), edt::prefab_document::context_name());
	BOOST_TEST(preview.focused);
	BOOST_TEST(preview.state.mouse[static_cast<std::size_t>(core::inp::MOUSE_BUTTONS::LEFT)]);
	const auto level = router.route("level", "main");
	BOOST_TEST(!level.focused);
	BOOST_TEST(!level.state.mouse[static_cast<std::size_t>(core::inp::MOUSE_BUTTONS::LEFT)]);
	click(false);

	move(ImVec2(initial.x + 200.f, initial.y + window->TitleBarHeight * 0.5f));
	click(true);
	move(ImVec2(initial.x + 240.f, initial.y + window->TitleBarHeight * 0.5f + 30.f));
	frame();
	BOOST_TEST(window->Pos.x == initial.x + 40.f);
	BOOST_TEST(window->Pos.y == initial.y + 30.f);
	click(false);
}

BOOST_AUTO_TEST_CASE(Open_CreatesASetOfItsOwnAndLeavesTheLevelAlone)
{
	BOOST_REQUIRE(mgr->load_level(put("lv.desc", LEVEL_TWO_WORLDS)));

	editor->open(prefab_a);

	BOOST_REQUIRE(editor->is_open());
	BOOST_TEST(editor->set_name() == "preview:" + std::string{ prefab_a.view() });
	auto* set = mgr->find_set(editor->set_name());
	BOOST_REQUIRE(set);
	BOOST_TEST(set->size() == 1u);
	auto* context = mgr->find(editor->set_name(), edt::prefab_document::context_name());
	BOOST_REQUIRE(context);
	BOOST_TEST((context->kind() == scn::context_kind::preview));
	BOOST_TEST((context->mode() == ecs::run_mode::edit));
	BOOST_TEST(editor->get_registry() == &context->state());
	BOOST_TEST(mgr->level_set().size() == 2u);
	BOOST_TEST(mgr->find("level", edt::prefab_document::context_name()) == nullptr);
	BOOST_TEST(!mgr->level_set().has(edt::prefab_document::context_name()));
}

BOOST_AUTO_TEST_CASE(Open_TheRegistryHasTheCameraTheLightAndThePrefab)
{
	editor->open(prefab_a);
	BOOST_REQUIRE(editor->is_open());
	auto* reg = editor->get_registry();
	BOOST_REQUIRE(reg);

	BOOST_TEST(reg->view<edt::prefab_camera_tag>().size() == 1u);
	BOOST_TEST(reg->view<scn::directional_light>().size() == 1u);
	bool prefab_spawned = false;
	for (auto [ent, name] : reg->view<scn::name_component>().each()) {
		if (name.name == "arm") prefab_spawned = true;
	}
	BOOST_TEST(prefab_spawned);
}

BOOST_AUTO_TEST_CASE(Close_DestroysTheSet)
{
	editor->open(prefab_a);
	const std::string set_name = editor->set_name();
	BOOST_REQUIRE(mgr->find_set(set_name));

	editor->close();

	BOOST_TEST(!editor->is_open());
	BOOST_TEST(mgr->find_set(set_name) == nullptr);
	BOOST_TEST(preview_sets() == 0u);
	BOOST_TEST(editor->get_registry() == nullptr);
	BOOST_TEST(editor->set_name().empty());
	BOOST_TEST(mgr->find_set("level") != nullptr);
}

BOOST_AUTO_TEST_CASE(Reopen_StartsFromAFreshPreview)
{
	editor->open(prefab_a);
	auto* first = editor->get_registry();
	BOOST_REQUIRE(first);
	(void)first->create(); // an edit that must not survive
	const auto entities_after_edit = first->storage<entt::entity>().size();

	editor->close();
	editor->open(prefab_a);

	BOOST_REQUIRE(editor->is_open());
	BOOST_TEST(preview_sets() == 1u);
	BOOST_REQUIRE(editor->get_registry());
	BOOST_TEST(editor->get_registry()->storage<entt::entity>().size() < entities_after_edit);
}

BOOST_AUTO_TEST_CASE(Open_AnotherPrefabWhileOpenSwitchesWithoutLeavingAStaleSet)
{
	editor->open(prefab_a);
	const std::string first_set = editor->set_name();

	editor->open(prefab_b);

	BOOST_REQUIRE(editor->is_open());
	BOOST_TEST(editor->set_name() != first_set);
	BOOST_TEST(mgr->find_set(first_set) == nullptr);
	BOOST_TEST(mgr->find_set(editor->set_name()) != nullptr);
	BOOST_TEST(preview_sets() == 1u);

	editor->close();
	BOOST_TEST(preview_sets() == 0u);
}

BOOST_AUTO_TEST_CASE(Open_OfAMissingPrefabCreatesNothing)
{
	editor->open(res::tag::make("prefabs/missing.prefab.desc"));

	BOOST_TEST(!editor->is_open());
	BOOST_TEST(preview_sets() == 0u);
}

BOOST_AUTO_TEST_CASE(Save_TheLevelFileDoesNotIncludeThePreview)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(mgr->load_level(tag));
	auto level_res = require_loaded<scn::level_desc>(tag);
	BOOST_REQUIRE(level_res.is_ready());
	// the editor's own load and save path (what Ctrl+S serializes), not a hand-filled list
	edt::scene_editor scene_ed(state, services);
	edt::world_controller world_ctrl(state, services, *rs, *descs);
	edt::level_controller level_ctrl(state, services, *rs, *descs, scene_ed, world_ctrl);
	level_ctrl.populate_worlds_from_level(level_res);
	editor->open(prefab_a);
	BOOST_REQUIRE(editor->is_open());

	const auto json = level_ctrl.build_level_json();
	const std::string text = boost::json::serialize(json);

	BOOST_TEST(text.find(std::string{ edt::prefab_document::context_name() }) == std::string::npos);
	BOOST_TEST(text.find("PrefabLight") == std::string::npos);
	BOOST_TEST(text.find("\"w0\"") != std::string::npos);
	BOOST_TEST(text.find("\"w1\"") != std::string::npos);
	mgr->get_set("level").for_each([](scn::world_context& context) {
		BOOST_TEST((context.kind() != scn::context_kind::preview));
	});
}

BOOST_AUTO_TEST_CASE(LevelReload_WhileThePreviewIsOpenKeepsItIntact)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(mgr->load_level(tag));
	editor->open(prefab_a);
	auto* registry = editor->get_registry();
	BOOST_REQUIRE(registry);
	const auto entities = registry->storage<entt::entity>().size();

	pump_reload(tag, LEVEL_ONE_WORLD); // another list of worlds: a full rebuild of the level

	BOOST_TEST(editor->is_open());
	BOOST_TEST(editor->get_registry() == registry);
	BOOST_TEST(registry->storage<entt::entity>().size() == entities);
	BOOST_TEST(preview_sets() == 1u);
	BOOST_TEST(mgr->level_set().size() == 1u);
	BOOST_TEST(mgr->level_set().has("only"));
}

BOOST_AUTO_TEST_CASE(LevelReload_ALevelWorldWithThePreviewNameDoesNotCollide)
{
	const auto tag = put("lv.desc", LEVEL_WITH_PREVIEW_NAME);
	BOOST_REQUIRE(mgr->load_level(tag));
	editor->open(prefab_a);
	BOOST_REQUIRE(editor->is_open());
	auto* preview = mgr->find_preview(prefab_a.view());
	auto* level_world = mgr->find("level", edt::prefab_document::context_name());
	BOOST_REQUIRE(preview);
	BOOST_REQUIRE(level_world);
	BOOST_TEST(preview != level_world);

	pump_reload(tag, LEVEL_TWO_WORLDS); // removes the level's namesake...
	BOOST_TEST(mgr->find_preview(prefab_a.view()) == preview);
	BOOST_TEST(mgr->find("level", edt::prefab_document::context_name()) == nullptr);

	pump_reload(tag, LEVEL_WITH_PREVIEW_NAME); // ...and brings it back
	BOOST_TEST(mgr->find_preview(prefab_a.view()) == preview);
	BOOST_TEST(mgr->find("level", edt::prefab_document::context_name()) != nullptr);
	BOOST_TEST(editor->get_registry() == &preview->state());
}

BOOST_AUTO_TEST_SUITE_END()
