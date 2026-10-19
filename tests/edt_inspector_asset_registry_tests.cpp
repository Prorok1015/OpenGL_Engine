#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "edt_builtin_asset_kinds.h"
#include "edt_inspector_panel.h"
#include "edt_scene_document.h"
#include "edt_panel_base.h"
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_inspector_registry.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <imgui.h>
#include <algorithm>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <concepts>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// inspector_registry and asset_kind_registry, the built-in asset kinds, the inspector's lookup through the
// registry and a test-only extension that puts one panel, one action, one inspector renderer and one asset kind
// into an editor_context and takes them out again (AC of #138). Not covered headless: the ImGui drawing of the
// asset browser and the viewport drop target.

namespace {

using comp_node = scn::prefab_desc::prefab_comp_node;

// Closed panels draw through on_hidden(), so counting renders needs no ImGui context.
struct ext_panel : edt::panel_base
{
	ext_panel() : panel_base("ext_panel", "Ext Panel") {}
	int hidden_count = 0;

protected:
	void on_render() override {}
	void on_hidden() override { ++hidden_count; }
};

edt::inspector_renderer returning(bool value, int* calls = nullptr)
{
	return [value, calls](comp_node&) {
		if (calls) ++*calls;
		return value;
	};
}

edt::asset_kind_desc kind_of(std::string id, std::string suffix, bool with_open, bool with_drop,
	std::vector<std::string>* log = nullptr)
{
	edt::asset_kind_desc desc;
	desc.id = std::move(id);
	desc.suffix = std::move(suffix);
	if (with_open) desc.open = [log, name = desc.id](edt::editor_context&, const res::tag&) { if (log) log->push_back("open:" + name); };
	if (with_drop) desc.drop_into_scene = [log, name = desc.id](edt::editor_context&, const res::tag&) { if (log) log->push_back("drop:" + name); };
	return desc;
}

// The engine on the null backend plus a real editor_context over it: the context only keeps references.
struct context_fixture : test_util::null_engine_fixture
{
	context_fixture()
		: null_engine_fixture(false)
		, gui(nullptr, nullptr)
		, context(std::make_unique<edt::editor_context>(
			storage.require<desc::desc_system>(), storage.require<res::resource_system>(), storage.require<rnd::render_system>(),
			gui, storage.require<scn::ecs_assembler>(), storage.require<ecs::system_factory>(),
			storage.require<scn::context_manager>(), tasks, commands, scene))
	{
	}

	gui::gui_system gui;
	cmd::command_registry commands;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
};

// Headless ImGui context: enough to run the inspector panel without a GPU.
struct imgui_ctx
{
	imgui_ctx()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~imgui_ctx() { ImGui::DestroyContext(ctx); }

	template<typename F>
	void frame(F&& body)
	{
		ImGui::NewFrame();
		body();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

scn::prefab_desc::prefab_node node_with(const std::string& type)
{
	scn::prefab_desc::prefab_node node;
	node.name = "n";
	comp_node comp;
	comp.type_name = type;
	comp.overrides["value"] = 60.0; // renderers are only invoked for components with overrides
	node.components[type] = comp;
	return node;
}

}

// The registries are reachable the same way from the editor_context.
static_assert(std::same_as<decltype(std::declval<edt::editor_context&>().inspectors()), edt::inspector_registry&>);
static_assert(std::same_as<decltype(std::declval<edt::editor_context&>().asset_kinds()), edt::asset_kind_registry&>);

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(EditorInspectorRegistryTests)

BOOST_AUTO_TEST_CASE(AddFindInvoke)
{
	edt::inspector_registry registry;
	auto reg = registry.add<scn::camera_desc>(returning(true));
	BOOST_CHECK(reg.active());
	BOOST_CHECK(registry.contains(scn::camera_desc::__type));
	BOOST_CHECK(registry.find(scn::camera_desc::__type) != nullptr);
	BOOST_CHECK_EQUAL(registry.size(), 1u);

	comp_node comp;
	const auto result = registry.invoke(scn::camera_desc::__type, comp);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(*result);
}

BOOST_AUTO_TEST_CASE(UnknownType_HasNoRenderer)
{
	edt::inspector_registry registry;
	comp_node comp;
	BOOST_CHECK(registry.find("nothing_desc") == nullptr);
	BOOST_CHECK(!registry.contains("nothing_desc"));
	BOOST_CHECK(!registry.invoke("nothing_desc", comp).has_value());
}

BOOST_AUTO_TEST_CASE(RegistrationOrderIsKept)
{
	edt::inspector_registry registry;
	auto r1 = registry.add("c_desc", returning(false));
	auto r2 = registry.add("a_desc", returning(false));
	auto r3 = registry.add("b_desc", returning(false));
	BOOST_CHECK((registry.types() == std::vector<std::string>{ "c_desc", "a_desc", "b_desc" }));
}

BOOST_AUTO_TEST_CASE(DuplicateType_IsRefusedAndTheFirstStays)
{
	edt::inspector_registry registry;
	int first_calls = 0;
	int second_calls = 0;
	auto first = registry.add("dup_desc", returning(true, &first_calls));
	auto second = registry.add("dup_desc", returning(true, &second_calls));
	BOOST_CHECK(first.active());
	BOOST_CHECK(!second.active());
	BOOST_CHECK_EQUAL(registry.size(), 1u);

	comp_node comp;
	registry.invoke("dup_desc", comp);
	BOOST_CHECK_EQUAL(first_calls, 1);
	BOOST_CHECK_EQUAL(second_calls, 0);

	// The refused registration does not remove the first one.
	second.reset();
	BOOST_CHECK(registry.contains("dup_desc"));
}

BOOST_AUTO_TEST_CASE(EmptyTypeOrNullRenderer_IsRefused)
{
	edt::inspector_registry registry;
	BOOST_CHECK(!registry.add("", returning(true)).active());
	BOOST_CHECK(!registry.add("null_desc", nullptr).active());
	BOOST_CHECK_EQUAL(registry.size(), 0u);
}

BOOST_AUTO_TEST_CASE(ResettingRegistration_RemovesTheRenderer)
{
	edt::inspector_registry registry;
	auto reg = registry.add("gone_desc", returning(true));
	reg.reset();
	BOOST_CHECK(!reg.active());
	BOOST_CHECK(!registry.contains("gone_desc"));
	BOOST_CHECK_NO_THROW(reg.reset());
}

BOOST_AUTO_TEST_CASE(DestroyedRegistration_RemovesTheRenderer)
{
	edt::inspector_registry registry;
	{
		auto reg = registry.add("scoped_desc", returning(true));
		BOOST_CHECK(registry.contains("scoped_desc"));
	}
	BOOST_CHECK(!registry.contains("scoped_desc"));
}

BOOST_AUTO_TEST_CASE(MovedRegistration_KeepsTheRendererUntilTheTargetDies)
{
	edt::inspector_registry registry;
	auto reg = registry.add("moved_desc", returning(true));
	{
		edt::registration target = std::move(reg);
		BOOST_CHECK(!reg.active());
		BOOST_CHECK(registry.contains("moved_desc"));
	}
	BOOST_CHECK(!registry.contains("moved_desc"));
}

BOOST_AUTO_TEST_CASE(RemoveByType_DropsTheRendererAndMakesTheRegistrationHarmless)
{
	edt::inspector_registry registry;
	auto reg = registry.add("x_desc", returning(true));
	BOOST_CHECK(registry.remove("x_desc"));
	BOOST_CHECK(!registry.remove("x_desc"));
	BOOST_CHECK(!registry.contains("x_desc"));

	// A renderer added later under the same type is not taken away by the stale registration.
	auto again = registry.add("x_desc", returning(false));
	BOOST_CHECK(again.active());
	reg.reset();
	BOOST_CHECK(registry.contains("x_desc"));
}

BOOST_AUTO_TEST_CASE(RegistrationOutlivingRegistry_IsHarmless)
{
	edt::registration reg;
	{
		edt::inspector_registry registry;
		reg = registry.add("late_desc", returning(true));
	}
	BOOST_CHECK_NO_THROW(reg.reset());
}

BOOST_AUTO_TEST_CASE(RendererMayRemoveItself)
{
	edt::inspector_registry registry;
	edt::registration reg;
	reg = registry.add("self_desc", [&reg](comp_node&) {
		reg.reset();
		return true;
	});
	comp_node comp;
	const auto result = registry.invoke("self_desc", comp);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(*result);
	BOOST_CHECK(!registry.contains("self_desc"));
}

// ─── the inspector panel looks renderers up in the registry ───

BOOST_AUTO_TEST_CASE(Panel_UsesTheRegisteredRenderer_AndTheFallbackOtherwise)
{
	imgui_ctx imgui;
	edt::inspector_registry registry;
	int camera_calls = 0;
	auto reg = registry.add<scn::camera_desc>(returning(true, &camera_calls));

	auto node = node_with(std::string(scn::camera_desc::__type));
	const auto plain = node_with("plain_desc");
	node.components["plain_desc"] = plain.components.at("plain_desc");

	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(camera_calls, 1); // the camera component has a renderer, plain_desc has none
	BOOST_CHECK_EQUAL(commits, 1);

	// Without the registration the camera component is drawn by the generic JSON renderer, which changes nothing.
	reg.reset();
	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(camera_calls, 1);
	BOOST_CHECK_EQUAL(commits, 1);
}

BOOST_AUTO_TEST_CASE(Panel_WithoutARegistry_UsesTheFallback)
{
	imgui_ctx imgui;
	auto node = node_with("plain_desc");
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_on_node_changed([&] { ++commits; });
	BOOST_CHECK_NO_THROW(imgui.frame([&] { panel.render(); }));
	BOOST_CHECK_EQUAL(commits, 0);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(EditorAssetKindRegistryTests)

BOOST_AUTO_TEST_CASE(AddFindAndRegistrationOrder)
{
	edt::asset_kind_registry registry;
	auto r1 = registry.add(kind_of("b", ".b", true, false));
	auto r2 = registry.add(kind_of("a", ".a", false, true));
	BOOST_CHECK(r1.active() && r2.active());
	BOOST_CHECK((registry.ids() == std::vector<std::string>{ "b", "a" }));
	BOOST_CHECK_EQUAL(registry.size(), 2u);
	BOOST_REQUIRE(registry.find("a") != nullptr);
	BOOST_CHECK_EQUAL(registry.find("a")->suffix, ".a");
	BOOST_CHECK(registry.find("missing") == nullptr);
}

BOOST_AUTO_TEST_CASE(InvalidKinds_AreRefused)
{
	edt::asset_kind_registry registry;
	BOOST_CHECK(!registry.add(kind_of("", ".x", true, true)).active());
	BOOST_CHECK(!registry.add(kind_of("no_action", ".x", false, false)).active());
	BOOST_CHECK_EQUAL(registry.size(), 0u);
}

BOOST_AUTO_TEST_CASE(DuplicateId_IsRefusedAndTheFirstStays)
{
	edt::asset_kind_registry registry;
	auto first = registry.add(kind_of("dup", ".one", true, false));
	auto second = registry.add(kind_of("dup", ".two", true, false));
	BOOST_CHECK(first.active());
	BOOST_CHECK(!second.active());
	BOOST_CHECK_EQUAL(registry.size(), 1u);
	BOOST_CHECK_EQUAL(registry.find("dup")->suffix, ".one");
	second.reset();
	BOOST_CHECK(registry.contains("dup"));
}

BOOST_AUTO_TEST_CASE(RegistrationRaii)
{
	edt::asset_kind_registry registry;
	{
		auto reg = registry.add(kind_of("scoped", ".s", true, false));
		BOOST_CHECK(registry.contains("scoped"));
		edt::registration moved = std::move(reg);
		BOOST_CHECK(!reg.active());
		BOOST_CHECK(registry.contains("scoped"));
	}
	BOOST_CHECK(!registry.contains("scoped"));

	edt::registration late;
	{
		edt::asset_kind_registry other;
		late = other.add(kind_of("late", ".l", true, false));
	}
	BOOST_CHECK_NO_THROW(late.reset());
}

BOOST_FIXTURE_TEST_CASE(Dispatch_LongestSuffixWins_TieGoesToTheEarlierKind, context_fixture)
{
	edt::asset_kind_registry registry;
	std::vector<std::string> log;
	auto generic = registry.add(kind_of("generic", ".desc", true, true, &log));
	auto prefab = registry.add(kind_of("prefab", ".prefab.desc", true, false, &log));
	auto twin = registry.add(kind_of("twin", ".desc", true, true, &log));

	BOOST_CHECK(registry.open(*context, res::tag::make("a/b.prefab.desc")));
	BOOST_CHECK(registry.open(*context, res::tag::make("a/b.desc")));
	// prefab has no drop: the longest suffix among the kinds that can handle a drop is used.
	BOOST_CHECK(registry.drop_into_scene(*context, res::tag::make("a/b.prefab.desc")));
	BOOST_CHECK((log == std::vector<std::string>{ "open:prefab", "open:generic", "drop:generic" }));

	log.clear();
	BOOST_CHECK(!registry.open(*context, res::tag::make("a/b.png")));
	BOOST_CHECK(!registry.drop_into_scene(*context, res::tag::make("a/b.png")));
	BOOST_CHECK(log.empty());
}

BOOST_FIXTURE_TEST_CASE(Dispatch_EmptySuffixMatchesEverything, context_fixture)
{
	edt::asset_kind_registry registry;
	std::vector<std::string> log;
	auto any = registry.add(kind_of("any", "", false, true, &log));
	BOOST_CHECK(registry.drop_into_scene(*context, res::tag::make("models/x.glb")));
	BOOST_CHECK(registry.drop_into_scene(*context, res::tag::make("x")));
	BOOST_CHECK(!registry.open(*context, res::tag::make("models/x.glb")));
	BOOST_CHECK_EQUAL(log.size(), 2u);
}

BOOST_FIXTURE_TEST_CASE(Dispatch_RemovedKindIsGone, context_fixture)
{
	edt::asset_kind_registry registry;
	std::vector<std::string> log;
	{
		auto reg = registry.add(kind_of("temp", ".tmp", true, false, &log));
		BOOST_CHECK(registry.open(*context, res::tag::make("a.tmp")));
	}
	BOOST_CHECK(!registry.open(*context, res::tag::make("a.tmp")));
	BOOST_CHECK_EQUAL(log.size(), 1u);
}

// ─── the kinds the editor registers itself ───

struct builtin_kinds_fixture : context_fixture
{
	builtin_kinds_fixture()
	{
		edt::asset_kind_hooks hooks;
		hooks.open_prefab = [this](const res::tag& tag) { log.push_back("prefab:" + std::string(tag.relative())); };
		hooks.add_desc_to_scene = [this](const res::tag& tag) { log.push_back("add:" + std::string(tag.relative())); };
		registrations = edt::register_builtin_asset_kinds(context->asset_kinds(), std::move(hooks));
	}

	std::vector<std::string> log;
	std::vector<edt::registration> registrations;
};

BOOST_FIXTURE_TEST_CASE(Builtin_PrefabSuffixOpensThePrefabDocument, builtin_kinds_fixture)
{
	auto& kinds = context->asset_kinds();
	BOOST_CHECK((kinds.ids() == std::vector<std::string>{ "prefab", "desc" }));
	BOOST_CHECK(kinds.open(*context, res::tag::make("prefabs/tree.prefab.desc")));
	BOOST_CHECK((log == std::vector<std::string>{ "prefab:prefabs/tree.prefab.desc" }));
}

BOOST_FIXTURE_TEST_CASE(Builtin_OtherAssetsAreNotOpened, builtin_kinds_fixture)
{
	auto& kinds = context->asset_kinds();
	for (const char* path : { "levels/a.desc", "models/x.glb", "x.prefab", "x.prefab.desc.bak" }) {
		BOOST_TEST_CONTEXT(path) {
			BOOST_CHECK(!kinds.open(*context, res::tag::make(path)));
		}
	}
	BOOST_CHECK(log.empty());
}

BOOST_FIXTURE_TEST_CASE(Builtin_EveryDroppedAssetIsAddedThroughItsDesc, builtin_kinds_fixture)
{
	auto& kinds = context->asset_kinds();
	for (const char* path : { "models/x.desc", "prefabs/tree.prefab.desc", "models/y.glb" }) {
		BOOST_CHECK(kinds.drop_into_scene(*context, res::tag::make(path)));
	}
	BOOST_CHECK((log == std::vector<std::string>{
		"add:models/x.desc", "add:prefabs/tree.prefab.desc", "add:models/y.glb" }));
}

BOOST_FIXTURE_TEST_CASE(Builtin_HandlesRemoveTheKinds, builtin_kinds_fixture)
{
	registrations.clear();
	BOOST_CHECK_EQUAL(context->asset_kinds().size(), 0u);
	BOOST_CHECK(!context->asset_kinds().drop_into_scene(*context, res::tag::make("models/x.desc")));
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────
// A test-only extension: it knows nothing but editor_context, as a plugin would.

namespace {

class test_extension
{
public:
	explicit test_extension(edt::editor_context& context)
		: m_context(context)
	{
		m_panel = std::make_shared<ext_panel>();
		m_registrations.push_back(context.panels().add({
			.id = "ext_panel", .title = "Ext Panel", .slot = edt::dock_slot::bottom_left, .open_by_default = false, .panel = m_panel }));

		edt::action_desc action;
		action.id = "ext.greet";
		action.label = "Greet";
		action.menu_path = "Ext";
		action.args_schema = boost::json::parse(
			R"({"type":"object","properties":{"name":{"type":"string","minLength":1}},"required":["name"],"additionalProperties":false})");
		action.result_schema = boost::json::parse(
			R"({"type":"object","properties":{"greeting":{"type":"string"}},"required":["greeting"],"additionalProperties":false})");
		action.execute = [this](const boost::json::value& args) {
			++greetings;
			const std::string name(args.as_object().at("name").as_string());
			return edt::action_result::success(boost::json::object{ { "greeting", "hello " + name } });
		};
		// A menu click would ask for the name through dialog_service; the test supplies one.
		action.activate = [this] { m_context.actions().invoke("ext.greet", boost::json::object{ { "name", "menu" } }); };
		m_registrations.push_back(context.actions().add(std::move(action)));

		m_registrations.push_back(context.inspectors().add("ext_desc", [](comp_node& comp) {
			comp.overrides["touched"] = true;
			return true;
		}));

		edt::asset_kind_desc kind;
		kind.id = "ext_kind";
		kind.suffix = ".ext";
		kind.icon = "[X]";
		kind.open = [this](edt::editor_context&, const res::tag& tag) { opened.push_back(std::string(tag.relative())); };
		kind.drop_into_scene = [this](edt::editor_context&, const res::tag& tag) { dropped.push_back(std::string(tag.relative())); };
		m_registrations.push_back(context.asset_kinds().add(std::move(kind)));
	}

	const std::shared_ptr<ext_panel>& panel() const { return m_panel; }
	std::size_t registration_count() const { return m_registrations.size(); }
	bool all_active() const
	{
		return std::all_of(m_registrations.begin(), m_registrations.end(), [](const edt::registration& r) { return r.active(); });
	}

	int greetings = 0;
	std::vector<std::string> opened;
	std::vector<std::string> dropped;

private:
	edt::editor_context& m_context;
	std::shared_ptr<ext_panel> m_panel;
	std::vector<edt::registration> m_registrations;
};

}

BOOST_FIXTURE_TEST_SUITE(EditorExtensionTests, context_fixture)

BOOST_AUTO_TEST_CASE(Extension_AppearsInPanelsMenusAndLookups)
{
	auto extension = std::make_unique<test_extension>(*context);
	BOOST_CHECK_EQUAL(extension->registration_count(), 4u);
	BOOST_CHECK(extension->all_active());

	// Panels: listed in the dock slot it asked for, drawn by render_all (closed: through on_hidden).
	auto& panels = context->panels();
	BOOST_CHECK(panels.contains("ext_panel"));
	const auto infos = panels.panels();
	BOOST_REQUIRE_EQUAL(infos.size(), 1u);
	BOOST_CHECK(infos[0].slot == edt::dock_slot::bottom_left);
	BOOST_CHECK(!infos[0].open);
	panels.render_all();
	BOOST_CHECK_EQUAL(extension->panel()->hidden_count, 1);

	// Actions: a menu item, a tool and a lookup.
	auto& actions = context->actions();
	BOOST_CHECK(actions.contains("ext.greet"));
	const auto menus = actions.menus();
	BOOST_CHECK(std::find(menus.begin(), menus.end(), "Ext") != menus.end());
	const auto items = actions.items("Ext");
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_CHECK_EQUAL(items[0].label, "Greet");
	BOOST_CHECK(items[0].enabled);
	const auto tools = actions.tools();
	BOOST_CHECK(std::any_of(tools.begin(), tools.end(), [](const cmd::command_desc* a) { return a->id == "ext.greet"; }));

	// Inspector renderer.
	auto& inspectors = context->inspectors();
	BOOST_CHECK(inspectors.contains("ext_desc"));
	comp_node comp;
	const auto drawn = inspectors.invoke("ext_desc", comp);
	BOOST_REQUIRE(drawn.has_value());
	BOOST_CHECK(*drawn);
	BOOST_CHECK(comp.overrides.contains("touched"));

	// Asset kind.
	auto& kinds = context->asset_kinds();
	BOOST_CHECK(kinds.contains("ext_kind"));
	BOOST_CHECK(kinds.open(*context, res::tag::make("things/a.ext")));
	BOOST_CHECK(kinds.drop_into_scene(*context, res::tag::make("things/b.ext")));
	BOOST_CHECK((extension->opened == std::vector<std::string>{ "things/a.ext" }));
	BOOST_CHECK((extension->dropped == std::vector<std::string>{ "things/b.ext" }));
}

BOOST_AUTO_TEST_CASE(Extension_ActionRunsThroughInvoke)
{
	auto extension = std::make_unique<test_extension>(*context);
	auto& actions = context->actions();

	const auto outcome = actions.invoke("ext.greet", boost::json::parse(R"({"name":"editor"})"));
	BOOST_CHECK(outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "ok");
	BOOST_CHECK(outcome.value.as_object().at("greeting").as_string() == "hello editor");
	BOOST_CHECK_EQUAL(extension->greetings, 1);

	// The schema is enforced before the action runs.
	for (const char* args : { "{}", R"({"name":""})", R"({"name":3})", R"({"name":"x","extra":1})" }) {
		BOOST_TEST_CONTEXT(args) {
			BOOST_CHECK_EQUAL(actions.invoke("ext.greet", boost::json::parse(args)).code, "invalid_args");
		}
	}
	BOOST_CHECK_EQUAL(extension->greetings, 1);

	// The menu click runs `activate`, which supplies the arguments.
	BOOST_CHECK(actions.activate("ext.greet"));
	BOOST_CHECK_EQUAL(extension->greetings, 2);
}

BOOST_AUTO_TEST_CASE(Extension_DestroyingItRemovesEverything)
{
	// A built-in entry registered earlier is not affected.
	auto builtin_renderer = context->inspectors().add("builtin_desc", [](comp_node&) { return false; });

	auto extension = std::make_unique<test_extension>(*context);
	BOOST_REQUIRE(context->panels().contains("ext_panel"));
	extension.reset();

	BOOST_CHECK(!context->panels().contains("ext_panel"));
	BOOST_CHECK_EQUAL(context->panels().size(), 0u);

	BOOST_CHECK(!context->actions().contains("ext.greet"));
	BOOST_CHECK_EQUAL(context->actions().invoke("ext.greet", boost::json::parse(R"({"name":"x"})")).code, "not_found");
	const auto menus = context->actions().menus();
	BOOST_CHECK(std::find(menus.begin(), menus.end(), "Ext") == menus.end());
	BOOST_CHECK(context->actions().items("Ext").empty());
	// What stays are the commands the context registers itself (job.*, dialog.*); none of the extension's.
	for (const auto* tool : context->actions().tools()) {
		BOOST_CHECK_MESSAGE(tool->id.rfind("ext.", 0) != 0, tool->id);
	}

	BOOST_CHECK(!context->inspectors().contains("ext_desc"));
	comp_node comp;
	BOOST_CHECK(!context->inspectors().invoke("ext_desc", comp).has_value());

	BOOST_CHECK(!context->asset_kinds().contains("ext_kind"));
	BOOST_CHECK_EQUAL(context->asset_kinds().size(), 0u);
	BOOST_CHECK(!context->asset_kinds().open(*context, res::tag::make("things/a.ext")));
	BOOST_CHECK(!context->asset_kinds().drop_into_scene(*context, res::tag::make("things/a.ext")));

	BOOST_CHECK(context->inspectors().contains("builtin_desc"));
}

BOOST_AUTO_TEST_CASE(Extension_TwoInstancesCollideAndTheSecondRegistersNothing)
{
	test_extension first(*context);
	test_extension second(*context);
	BOOST_CHECK(first.all_active());
	BOOST_CHECK(!second.all_active());

	// The second extension's refused entries must not remove the first one's when it goes away.
	{
		test_extension third(*context);
	}
	BOOST_CHECK(context->panels().contains("ext_panel"));
	BOOST_CHECK(context->actions().contains("ext.greet"));
	BOOST_CHECK(context->inspectors().contains("ext_desc"));
	BOOST_CHECK(context->asset_kinds().contains("ext_kind"));
}

BOOST_AUTO_TEST_SUITE_END()
