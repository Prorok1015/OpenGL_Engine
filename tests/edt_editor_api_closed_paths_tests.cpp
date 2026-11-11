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

using document_signal_handler = std::function<void(edt::document_id, std::uint64_t)>;

// The module's events() reads: a subscription by hand (the connection would be the module's to drop) does not compile.
template<class T>
concept subscribes_by_hand = requires(T& c, document_signal_handler handler) { c.events().document_changed.subscribe(handler); };
template<class T>
concept subscribes_through_the_context = requires(T& c, document_signal_handler handler) { c.subscribe(c.events().document_changed, handler); };

template<class T>
concept registers_a_host_asset_kind = requires(T& c, edt::asset_kind_desc desc) { c.add_asset_kind(std::move(desc)); };
template<class T>
concept registers_a_host_asset_preview = requires(T& c, edt::asset_preview_desc desc) { c.add_asset_preview(std::move(desc)); };

}

BOOST_AUTO_TEST_SUITE(EdtEditorApiClosedPathsTests)

BOOST_AUTO_TEST_CASE(TheEnginesServices_AreNotHandedToAModule)
{
	static_assert(!has_services<context>, "editor_module_context::services() is gone (2.0): the registries of editor_services take descs and spawners past the scope");
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

BOOST_AUTO_TEST_CASE(TheEventsOfTheEditor_AreReadOnly_AndASubscriptionGoesThroughTheContext)
{
	static_assert(std::is_same_v<decltype(std::declval<context&>().events()), const edt::editor_events&>);
	static_assert(!subscribes_by_hand<context>, "events().x.subscribe(...) must not compile: the connection would not be in the scope");
	static_assert(subscribes_through_the_context<context>);
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
