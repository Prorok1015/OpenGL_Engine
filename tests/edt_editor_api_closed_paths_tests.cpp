#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_editor_extension.h>
#include <functional>
#include <type_traits>

// #663 (editor API 2.0): what a game_editor module can reach through editor_module_context. Every check is a compile-time
// fact: a path that was closed stays closed, and the build breaks when someone reopens it. The guarantee is about what
// the API offers, not about the language (a module that includes the private headers is outside it).

namespace {

using context = edt::editor_module_context;

template<class T>
concept has_services = requires(T& c) { c.services(); };
template<class T>
concept has_dialogs = requires(T& c) { c.dialogs(); };
template<class T>
concept has_editor_context = requires(T& c) { c.editor(); } || requires(T& c) { c.context(); };
template<class T>
concept has_asset_kinds = requires(T& c) { c.asset_kinds(); };
template<class T>
concept has_panels = requires(T& c) { c.panels(); };
template<class T>
concept has_actions = requires(T& c) { c.actions(); };
template<class T>
concept has_inspectors = requires(T& c) { c.inspectors(); };
template<class T>
concept has_resources = requires(T& c) { c.resources(); };
template<class T>
concept has_render = requires(T& c) { c.render(); };
template<class T>
concept has_gui = requires(T& c) { c.gui(); };
template<class T>
concept has_task_system = requires(T& c) { c.task_system(); };
template<class T>
concept has_events = requires(T& c) { c.events(); };

// The module's own tasks: bound to its scope (the owner is not a parameter).
template<class T>
concept runs_a_task = requires(T& c) { c.run(ds::lane::compute, ds::task_desc{ "t" }, [](ds::task_context&) {}); };

// The module reaches an event only through subscribe(pointer to member, handler): the connection goes into the scope.
using document_signal_handler = std::function<void(edt::document_id, std::uint64_t)>;
template<class T>
concept subscribes_through_the_context = requires(T& c, document_signal_handler handler) { c.subscribe(&edt::editor_events::document_changed, handler); };

// What documents() and selection() let a module do: read. Each of these once let a module hand a host object a pointer into itself.
template<class T>
concept sets_a_registry_provider = requires(T& c) { c.selection().set_registry_provider({}); };
template<class T>
concept sets_preview_kinds = requires(T& c, edt::asset_kind_registry& kinds, edt::editor_services& services) { c.documents().set_preview_kinds(kinds, services); };
template<class T>
concept binds_the_scene_to_events = requires(T& c, edt::editor_events* events) { c.documents().scene().bind_events(events); };
template<class T>
concept opens_a_preview = requires(T& c) { c.documents().open_preview(res::tag{ "res://a" }); };
template<class T>
concept selects_through_the_service = requires(T& c) { c.selection().clear(); };

template<class T>
concept registers_a_host_asset_kind = requires(T& c, edt::asset_kind_desc desc) { c.add_asset_kind(std::move(desc)); };
template<class T>
concept registers_a_host_asset_preview = requires(T& c, edt::asset_preview_desc desc) { c.add_asset_preview(std::move(desc)); };

}

BOOST_AUTO_TEST_SUITE(EdtEditorApiClosedPathsTests)

BOOST_AUTO_TEST_CASE(TheEnginesServices_AreNotHandedToAModule)
{
	static_assert(!has_services<context>, "editor_module_context::services() is gone (2.0): the registries of editor_services take descs and spawners past the scope");
	static_assert(!has_resources<context>, "resources(): register_mount, register_alias, add_mount, subscribe_changes register past the scope");
	static_assert(!has_render<context>, "render(): add_render_pass, set_material_compiler register past the scope");
	static_assert(!has_gui<context>, "gui(): push_layer registers past the scope");
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(TheDialogServiceTheEditorContextAndTheRegistries_AreNotHandedToAModule)
{
	static_assert(!has_dialogs<context>, "a dialog's callbacks would outlive the module");
	static_assert(!has_editor_context<context>, "the editor_context is the host's: its registries register past the scope");
	static_assert(!has_asset_kinds<context> && !has_panels<context> && !has_actions<context> && !has_inspectors<context>,
		"a registry is reached through add_*, which keeps the entry in the scope");
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(TheEventsOfTheEditor_AreNotHandedToAModule_ItReachesThemOnlyThroughSubscribe)
{
	static_assert(!has_events<context>, "events(): a module could subscribe by hand (a connection that outlives it) and emit agent_action, play_state_changed, game_module_built");
	static_assert(subscribes_through_the_context<context>);
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(TheTaskSystem_IsNotHandedToAModule_ItsTasksAreBoundToItsScope)
{
	static_assert(!has_task_system<context>, "run without an owner and post_main queue code that stop_tasks() does not stop");
	static_assert(runs_a_task<context>);
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(TheDocumentsAndTheSelection_AreReadOnlyForAModule)
{
	static_assert(std::is_same_v<decltype(std::declval<context&>().documents()), const edt::document_manager&>);
	static_assert(std::is_same_v<decltype(std::declval<context&>().selection()), const edt::selection_service&>);
	static_assert(!sets_a_registry_provider<context>, "a std::function into the module's code, called after the unload");
	static_assert(!sets_preview_kinds<context>, "host pointers into the module's memory");
	static_assert(!binds_the_scene_to_events<context>);
	static_assert(!opens_a_preview<context>);
	static_assert(!selects_through_the_service<context>);
	static_assert(requires(context& c, const edt::selection& value) { c.select(value); c.clear_selection(); });
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(AnAssetKindOrPreviewOfAModule_TakesTheModuleDescsOnly_WhoseCallbacksGetTheModuleContext)
{
	static_assert(!registers_a_host_asset_kind<context>, "asset_kind_desc's callbacks get the editor_context");
	static_assert(!registers_a_host_asset_preview<context>, "asset_preview_desc's factory gets the editor_services");

	static_assert(std::is_invocable_v<decltype(edt::module_asset_kind_desc::open), edt::editor_module_context&, const res::tag&>);
	static_assert(!std::is_invocable_v<decltype(edt::module_asset_kind_desc::open), edt::editor_context&, const res::tag&>);
	static_assert(!std::is_invocable_v<decltype(edt::module_asset_kind_desc::drop_into_scene), edt::editor_context&, const res::tag&>);
	static_assert(std::is_invocable_v<decltype(edt::module_asset_preview_desc::make_preview), edt::editor_module_context&>);
	static_assert(!std::is_invocable_v<decltype(edt::module_asset_preview_desc::make_preview), edt::editor_services&>);
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(TheEditorApiVersion_IsTheMajorOf2)
{
	BOOST_CHECK_EQUAL(app::modules::api_major(SNK_EDITOR_API_VERSION), 2u);
	BOOST_CHECK_EQUAL(app::modules::api_minor(SNK_EDITOR_API_VERSION), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
