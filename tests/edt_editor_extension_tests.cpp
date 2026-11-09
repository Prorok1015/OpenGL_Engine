#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include "null_engine_fixture.hpp"
#include "edt_document_cast.h"
#include "edt_scene_document.h"
#include "test_manual_tasks.hpp"
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_inspector_registry.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_system.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <memory>

// What a game_editor module (#588) leaves behind: nothing. A module registers only through its context; unloading it
// puts the registries, the subscriptions and the tasks back as they were, and a module of another API major version
// is refused by the host's check. The pattern is app_game_module_tests/unloading_leaves_the_root_and_the_base_as_they_were.

namespace {

class probe_panel final : public edt::panel_base
{
public:
	probe_panel() : panel_base("ext.panel", "Ext") {}

protected:
	void on_render() override {}
};

struct probe_state
{
	int document_changes = 0;
	int job_bodies_run = 0;
	int job_results_delivered = 0;
	int continuations_run = 0;
	int shutdowns = 0;
};

class probe_extension final : public edt::editor_extension
{
public:
	explicit probe_extension(probe_state& state) : m_state(state) {}

	std::string_view name() const override { return "probe_editor"; }

	void register_editor(edt::editor_module_context& context) override
	{
		edt::panel_desc panel;
		panel.id = "ext.panel";
		panel.title = "Ext";
		panel.open_by_default = false;
		panel.show_in_view_menu = false;
		panel.panel = std::make_shared<probe_panel>();
		context.add_panel(std::move(panel));

		context.add_menu("Ext");
		edt::action_desc action;
		action.id = "ext.run";
		action.label = "Run";
		action.menu_path = "Ext";
		action.execute = edt::no_args([] {});
		context.add_action(std::move(action));

		context.add_inspector("ext_component", [](scn::prefab_desc::prefab_comp_node&) { return false; });

		edt::asset_kind_desc kind;
		kind.id = "ext.kind";
		kind.suffix = ".ext";
		context.add_asset_kind(std::move(kind));

		context.subscribe(context.events().document_changed, [this](edt::document_id, std::uint64_t) { ++m_state.document_changes; });

		probe_state& state = m_state;
		context.jobs().start(ds::lane::compute, ds::task_desc{ "probe job" }, context.tasks(),
			[&state](ds::task_context&) {
				++state.job_bodies_run;
				return 0;
			},
			[&state](ds::task_result<int>&) { ++state.job_results_delivered; });
	}

	void shutdown_editor() override { ++m_state.shutdowns; }

private:
	probe_state& m_state;
};

struct extension_fixture : test_util::null_engine_fixture
{
	extension_fixture()
		: null_engine_fixture(false)
		, gui(nullptr, nullptr)
		, services{ .ctx_manager = storage.require_shared<scn::context_manager>(),
			.base_descs = &storage.require<desc::desc_system>(), .base_resources = &storage.require<res::resource_system>(),
			.base_assembler = &storage.require<scn::ecs_assembler>(), .base_sfactory = &storage.require<ecs::system_factory>() }
		, context(std::make_unique<edt::editor_context>(
			storage.require<res::resource_system>(), storage.require<rnd::render_system>(),
			gui, services, tasks, commands, scene, edt::prefab_preview(services)))
	{
		host_data.attach(std::shared_ptr<edt::editor_context>(context.get(), [](edt::editor_context*) {}));
	}

	~extension_fixture()
	{
		context.reset();
	}

	struct counts
	{
		std::size_t panels = 0;
		std::size_t actions = 0;
		std::size_t menus = 0;
		std::size_t inspectors = 0;
		std::size_t asset_kinds = 0;
		std::size_t document_subscribers = 0;

		bool operator==(const counts&) const = default;
	};

	counts snapshot()
	{
		return { context->panels().size(), context->actions().size(), context->actions().menus().size(),
			context->inspectors().size(), context->asset_kinds().size(), context->events().document_changed.subscriber_count() };
	}

	gui::gui_system gui;
	cmd::command_registry commands;
	edt::editor_services services;
	edt::scene_document scene;
	std::unique_ptr<edt::editor_context> context;
	ds::app_data_storage host_data;
	probe_state state;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtEditorExtensionTests, extension_fixture)

BOOST_AUTO_TEST_CASE(LoadingAModule_RegistersThroughItsContext_AndUnloadingLeavesTheEditorAsItWas)
{
	const counts before = snapshot();
	{
		probe_extension module(state);
		module.register_services(host_data);
		module.initialize_services(host_data);

		BOOST_CHECK(context->panels().contains("ext.panel"));
		BOOST_CHECK(context->actions().find("ext.run") != nullptr);
		BOOST_CHECK(context->asset_kinds().find("ext.kind") != nullptr);
		BOOST_CHECK_EQUAL(context->panels().size(), before.panels + 1);
		BOOST_CHECK_EQUAL(context->actions().size(), before.actions + 1);
		BOOST_CHECK_EQUAL(context->inspectors().size(), before.inspectors + 1);
		BOOST_CHECK_EQUAL(context->asset_kinds().size(), before.asset_kinds + 1);
		BOOST_CHECK_EQUAL(context->events().document_changed.subscriber_count(), before.document_subscribers + 1);

		module.shutdown_services(host_data);
		BOOST_CHECK_EQUAL(state.shutdowns, 1);
	}
	BOOST_CHECK(snapshot() == before);
	BOOST_CHECK(context->actions().find("ext.run") == nullptr);
}

BOOST_AUTO_TEST_CASE(AModuleLoadedAgain_RegistersWithoutClashingWithTheFirstLoad)
{
	const counts before = snapshot();
	for (int round = 0; round < 2; ++round) {
		probe_extension module(state);
		module.register_services(host_data);
		BOOST_CHECK(context->panels().contains("ext.panel"));
		module.shutdown_services(host_data);
	}
	BOOST_CHECK(snapshot() == before);
}

BOOST_AUTO_TEST_CASE(TheJobsOfALoadedModule_Run_SoTheUnloadTestBelowCanFail)
{
	probe_extension module(state);
	module.register_services(host_data);
	test_util::pump(tasks);
	test_util::pump(tasks);
	BOOST_CHECK_EQUAL(state.job_bodies_run, 1);
	BOOST_CHECK_EQUAL(state.job_results_delivered, 1);
	module.shutdown_services(host_data);
}

BOOST_AUTO_TEST_CASE(TheJobsOfAModule_AreStoppedOnUnload_AndNothingOfThemRunsAfterwards)
{
	{
		probe_extension module(state);
		module.register_services(host_data);
		module.shutdown_services(host_data);
	}
	test_util::pump(tasks);
	test_util::pump(tasks);
	BOOST_CHECK_EQUAL(state.job_bodies_run, 0);
	BOOST_CHECK_EQUAL(state.job_results_delivered, 0);
}

BOOST_AUTO_TEST_CASE(ASubscriptionOfAModule_IsGoneAfterUnload_SoTheEventNoLongerReachesIt)
{
	{
		probe_extension module(state);
		module.register_services(host_data);
		context->events().document_changed.emit(edt::document_id{}, 1);
		BOOST_CHECK_EQUAL(state.document_changes, 1);
		module.shutdown_services(host_data);
	}
	context->events().document_changed.emit(edt::document_id{}, 2);
	BOOST_CHECK_EQUAL(state.document_changes, 1);
}

BOOST_AUTO_TEST_CASE(AModuleThatIsDestroyedWithoutShutdown_StillLeavesNoTrace)
{
	const counts before = snapshot();
	{
		probe_extension module(state);
		module.register_services(host_data);
	}
	BOOST_CHECK(snapshot() == before);
}

BOOST_AUTO_TEST_CASE(TheHostRefusesAModuleOfAnotherMajorVersion_AndAcceptsOneItCanServe)
{
	const std::uint32_t host = SNK_EDITOR_API_VERSION;
	static std::uint32_t reported = 0;
	reported = host;
	BOOST_CHECK(edt::check_editor_api_version([]() -> std::uint32_t { return reported; }).empty());

	reported = host + (1u << 16);
	const std::string newer_major = edt::check_editor_api_version([]() -> std::uint32_t { return reported; });
	BOOST_CHECK_MESSAGE(newer_major.find("major") != std::string::npos, newer_major);

	reported = host + 1u;
	const std::string newer_minor = edt::check_editor_api_version([]() -> std::uint32_t { return reported; });
	BOOST_CHECK_MESSAGE(newer_minor.find("minor") != std::string::npos, newer_minor);

	BOOST_CHECK(!edt::check_editor_api_version(nullptr).empty());
}

BOOST_AUTO_TEST_CASE(ARefusedModule_TouchesNothing_SoTheGameKeepsWorking)
{
	const counts before = snapshot();
	static std::uint32_t reported = SNK_EDITOR_API_VERSION + (1u << 16);
	const std::string reason = edt::check_editor_api_version([]() -> std::uint32_t { return reported; });
	BOOST_CHECK(!reason.empty());
	BOOST_CHECK(snapshot() == before);
}

BOOST_AUTO_TEST_SUITE_END()
