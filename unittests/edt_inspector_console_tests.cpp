#include <boost/test/unit_test.hpp>
#include "edt_inspector_panel.h"
#include "edt_console_panel.h"
#include <entt/entt.hpp>

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(InspectorPanelTests)

BOOST_AUTO_TEST_CASE(DefaultConstruct_DoesNotCrash)
{
	BOOST_CHECK_NO_THROW(edt::inspector_panel panel);
}

BOOST_AUTO_TEST_CASE(SetRegistry_DoesNotCrash)
{
	edt::inspector_panel panel;
	auto registry = std::make_shared<entt::registry>();
	BOOST_CHECK_NO_THROW(panel.set_registry(registry));
}

BOOST_AUTO_TEST_CASE(SetSelectedEntity_NullDoesNotCrash)
{
	edt::inspector_panel panel;
	auto registry = std::make_shared<entt::registry>();
	panel.set_registry(registry);
	BOOST_CHECK_NO_THROW(panel.set_selected_entity(entt::null));
}

BOOST_AUTO_TEST_CASE(SetSelectedEntity_ValidEntity)
{
	edt::inspector_panel panel;
	auto registry = std::make_shared<entt::registry>();
	panel.set_registry(registry);

	auto ent = registry->create();
	BOOST_CHECK_NO_THROW(panel.set_selected_entity(ent));
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(ConsolePanelTests)

BOOST_AUTO_TEST_CASE(DefaultConstruct_Empty)
{
	edt::console_panel panel;
	BOOST_CHECK_EQUAL(panel.get_log_count(), 0u);
	BOOST_CHECK_EQUAL(panel.get_error_count(), 0u);
	BOOST_CHECK_EQUAL(panel.get_warning_count(), 0u);
}

BOOST_AUTO_TEST_CASE(AddLog_IncreasesCount)
{
	edt::console_panel panel;
	panel.add_log(edt::log_level::info, "Hello");
	BOOST_CHECK_EQUAL(panel.get_log_count(), 1u);

	panel.add_log(edt::log_level::warning, "Watch out");
	panel.add_log(edt::log_level::error, "Boom");
	BOOST_CHECK_EQUAL(panel.get_log_count(), 3u);
}

BOOST_AUTO_TEST_CASE(ErrorCount_TrackedSeparately)
{
	edt::console_panel panel;
	panel.add_log(edt::log_level::info,    "i1");
	panel.add_log(edt::log_level::warning, "w1");
	panel.add_log(edt::log_level::error,   "e1");
	panel.add_log(edt::log_level::error,   "e2");

	BOOST_CHECK_EQUAL(panel.get_error_count(),   2u);
	BOOST_CHECK_EQUAL(panel.get_warning_count(), 1u);
}

BOOST_AUTO_TEST_CASE(ClearLogs_EmptiesAll)
{
	edt::console_panel panel;
	panel.add_log(edt::log_level::info,  "msg");
	panel.add_log(edt::log_level::error, "err");
	panel.clear();

	BOOST_CHECK_EQUAL(panel.get_log_count(),   0u);
	BOOST_CHECK_EQUAL(panel.get_error_count(), 0u);
}

BOOST_AUTO_TEST_CASE(AddManyLogs_StressTest)
{
	edt::console_panel panel;
	for (int i = 0; i < 1000; ++i)
		panel.add_log(edt::log_level::info, "msg " + std::to_string(i));
	BOOST_CHECK_EQUAL(panel.get_log_count(), 1000u);
}

BOOST_AUTO_TEST_SUITE_END()
