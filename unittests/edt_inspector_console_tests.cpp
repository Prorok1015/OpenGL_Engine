#include <boost/test/unit_test.hpp>
#include "edt_inspector_panel.h"
#include "edt_component_ui_registry.h"
#include "edt_console_panel.h"
#include "level/scn_prefab_desc.h"
#include "scn_camera_desc.h"
#include "scn_directional_light_desc.h"
#include "scn_skybox_desc.h"

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(InspectorPanelTests)

BOOST_AUTO_TEST_CASE(DefaultConstruct_DoesNotCrash)
{
	BOOST_CHECK_NO_THROW(edt::inspector_panel panel);
}

BOOST_AUTO_TEST_CASE(SetSelectedNode_Null_DoesNotCrash)
{
	edt::inspector_panel panel;
	BOOST_CHECK_NO_THROW(panel.set_selected_node(nullptr));
}

BOOST_AUTO_TEST_CASE(SetSelectedNode_ValidNode_DoesNotCrash)
{
	edt::inspector_panel panel;
	scn::prefab_desc::prefab_node node;
	node.name = "TestNode";
	BOOST_CHECK_NO_THROW(panel.set_selected_node(&node));
}

BOOST_AUTO_TEST_CASE(SetOnNodeChanged_Stored)
{
	edt::inspector_panel panel;
	bool called = false;
	panel.set_on_node_changed([&]() { called = true; });
	BOOST_CHECK(!called);
}

BOOST_AUTO_TEST_CASE(RegisterRenderer_DoesNotCrash)
{
	edt::component_ui_registry registry;
	bool renderer_called = false;
	registry.register_renderer<scn::camera_desc>(
		[&](scn::prefab_desc::prefab_comp_node&) -> bool {
			renderer_called = true;
			return false;
		});
	BOOST_CHECK(!renderer_called); // не вызывается без UI
}

BOOST_AUTO_TEST_CASE(RegisterMultipleRenderers_DoesNotCrash)
{
	edt::component_ui_registry registry;
	registry.register_renderer<scn::camera_desc>(           [](scn::prefab_desc::prefab_comp_node&) { return false; });
	registry.register_renderer<scn::directional_light_desc>([](scn::prefab_desc::prefab_comp_node&) { return false; });
	registry.register_renderer<scn::skybox_desc>(           [](scn::prefab_desc::prefab_comp_node&) { return false; });
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(InvokeRenderer_KnownType_ReturnsValue)
{
	edt::component_ui_registry registry;
	registry.register_renderer<scn::camera_desc>([](scn::prefab_desc::prefab_comp_node&) { return true; });
	scn::prefab_desc::prefab_comp_node comp;
	auto result = registry.invoke(scn::camera_desc::__type, comp);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(*result == true);
}

BOOST_AUTO_TEST_CASE(InvokeRenderer_UnknownType_ReturnsNullopt)
{
	edt::component_ui_registry registry;
	scn::prefab_desc::prefab_comp_node comp;
	auto result = registry.invoke("unknown_desc", comp);
	BOOST_CHECK(!result.has_value());
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
