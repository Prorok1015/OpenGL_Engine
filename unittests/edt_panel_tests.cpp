#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include "edt_panel_manager.h"
#include "edt_scene_hierarchy_panel.h"
#include <entt/entt.hpp>

// Mock-панель: наследует panel_base, НЕ вызывает ImGui
struct mock_panel : public edt::panel_base
{
	mock_panel(std::string_view id, std::string_view title)
		: panel_base(id, title) {}

	int render_count = 0;

protected:
	void on_render() override { render_count++; }
};

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(EditorPanelManagerTests)

BOOST_AUTO_TEST_CASE(AddAndFindPanel)
{
	edt::panel_manager pm;
	pm.add_panel(std::make_shared<mock_panel>("test_panel", "Test"));
	auto* found = pm.find_panel("test_panel");
	BOOST_REQUIRE(found != nullptr);
	BOOST_CHECK_EQUAL(found->get_id(), "test_panel");
}

BOOST_AUTO_TEST_CASE(FindMissingPanel_ReturnsNull)
{
	edt::panel_manager pm;
	BOOST_CHECK(pm.find_panel("nonexistent") == nullptr);
}

BOOST_AUTO_TEST_CASE(RemovePanel)
{
	edt::panel_manager pm;
	pm.add_panel(std::make_shared<mock_panel>("to_remove", "Remove"));
	pm.remove_panel("to_remove");
	BOOST_CHECK(pm.find_panel("to_remove") == nullptr);
}

BOOST_AUTO_TEST_CASE(RemoveMissingPanel_DoesNotCrash)
{
	edt::panel_manager pm;
	BOOST_CHECK_NO_THROW(pm.remove_panel("ghost"));
}

BOOST_AUTO_TEST_CASE(PanelOpenClose)
{
	edt::panel_manager pm;
	auto panel = std::make_shared<mock_panel>("toggle_panel", "Toggle");
	pm.add_panel(panel);

	BOOST_CHECK(panel->is_open());
	panel->set_open(false);
	BOOST_CHECK(!panel->is_open());
	panel->set_open(true);
	BOOST_CHECK(panel->is_open());
}

BOOST_AUTO_TEST_CASE(AddMultiplePanels)
{
	edt::panel_manager pm;
	pm.add_panel(std::make_shared<mock_panel>("a", "A"));
	pm.add_panel(std::make_shared<mock_panel>("b", "B"));
	pm.add_panel(std::make_shared<mock_panel>("c", "C"));

	BOOST_CHECK(pm.find_panel("a") != nullptr);
	BOOST_CHECK(pm.find_panel("b") != nullptr);
	BOOST_CHECK(pm.find_panel("c") != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(SceneHierarchyPanelTests)

BOOST_AUTO_TEST_CASE(DefaultSelectedEntity_IsNull)
{
	edt::scene_hierarchy_panel panel;
	BOOST_CHECK(panel.get_selected_entity() == entt::null);
}

BOOST_AUTO_TEST_CASE(SetRegistry_DoesNotCrash)
{
	edt::scene_hierarchy_panel panel;
	auto registry = std::make_shared<entt::registry>();
	BOOST_CHECK_NO_THROW(panel.set_registry(registry));
}

BOOST_AUTO_TEST_CASE(SetSelectedEntity_TrackedCorrectly)
{
	edt::scene_hierarchy_panel panel;
	auto registry = std::make_shared<entt::registry>();
	panel.set_registry(registry);

	auto ent = registry->create();
	panel.set_selected_entity(ent);
	BOOST_CHECK(panel.get_selected_entity() == ent);
}

BOOST_AUTO_TEST_CASE(SetSelectedEntity_ToNull)
{
	edt::scene_hierarchy_panel panel;
	auto registry = std::make_shared<entt::registry>();
	panel.set_registry(registry);

	auto ent = registry->create();
	panel.set_selected_entity(ent);
	panel.set_selected_entity(entt::null);
	BOOST_CHECK(panel.get_selected_entity() == entt::null);
}

BOOST_AUTO_TEST_CASE(OnEntitySelectedCallback_Stored)
{
	edt::scene_hierarchy_panel panel;
	bool called = false;
	panel.set_on_entity_selected([&](ecs::entity) { called = true; });
	// Callback сохранён — он вызовется при клике в UI
	// Здесь просто проверяем что установка не падает
	BOOST_CHECK(!called); // не вызван без UI
}

BOOST_AUTO_TEST_SUITE_END()
