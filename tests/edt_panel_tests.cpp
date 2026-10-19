#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_panel_registry.h>
#include "edt_panel_base.h"
#include "edt_scene_hierarchy_panel.h"
#include <snk/scene/level/scn_world_desc.h>
#include "edt_editor_layer.h"
#include <snk/gui/gui_debug_layer.h>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// Layers are owned via shared_ptr<gui::layer_interface>; a non-virtual dtor would be UB (#173).
static_assert(std::has_virtual_destructor_v<gui::layer_interface>);
static_assert(std::has_virtual_destructor_v<gui::debug_layer>);
static_assert(std::has_virtual_destructor_v<edt::editor_layer>);

// Mock-панель: наследует panel_base, НЕ вызывает ImGui. Закрытая панель рисуется через on_hidden(),
// поэтому render_all() считает вызовы без ImGui-контекста.
struct mock_panel : public edt::panel_base
{
	mock_panel(std::string_view id, std::string_view title, std::vector<std::string>* log = nullptr)
		: panel_base(id, title), m_log(log) {}

	int hidden_count = 0;

protected:
	void on_render() override {}
	void on_hidden() override
	{
		++hidden_count;
		if (m_log) m_log->push_back(m_id);
	}

private:
	std::vector<std::string>* m_log;
};

namespace {
	edt::panel_desc desc_of(const std::shared_ptr<mock_panel>& panel, edt::dock_slot slot = edt::dock_slot::floating, bool open = true)
	{
		return { .id = panel->get_id(), .title = panel->get_title(), .slot = slot, .open_by_default = open, .panel = panel };
	}

	std::vector<std::string> ids_of(const edt::panel_registry& registry)
	{
		std::vector<std::string> ids;
		for (const auto& info : registry.panels()) ids.push_back(info.id);
		return ids;
	}
}

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(EditorPanelRegistryTests)

BOOST_AUTO_TEST_CASE(AddAndFindPanel)
{
	edt::panel_registry registry;
	auto panel = std::make_shared<mock_panel>("test_panel", "Test");
	auto reg = registry.add(desc_of(panel));
	BOOST_CHECK(reg.active());
	BOOST_REQUIRE(registry.find("test_panel") == panel.get());
	BOOST_CHECK(registry.contains("test_panel"));
	BOOST_CHECK_EQUAL(registry.size(), 1u);
}

BOOST_AUTO_TEST_CASE(FindMissingPanel_ReturnsNull)
{
	edt::panel_registry registry;
	BOOST_CHECK(registry.find("nonexistent") == nullptr);
	BOOST_CHECK(!registry.contains("nonexistent"));
}

BOOST_AUTO_TEST_CASE(ResettingRegistration_RemovesPanel)
{
	edt::panel_registry registry;
	auto reg = registry.add(desc_of(std::make_shared<mock_panel>("to_remove", "Remove")));
	reg.reset();
	BOOST_CHECK(!reg.active());
	BOOST_CHECK(registry.find("to_remove") == nullptr);
	BOOST_CHECK_EQUAL(registry.size(), 0u);
	BOOST_CHECK_NO_THROW(reg.reset());
}

BOOST_AUTO_TEST_CASE(DestroyedRegistration_RemovesPanel)
{
	edt::panel_registry registry;
	{
		auto reg = registry.add(desc_of(std::make_shared<mock_panel>("scoped", "Scoped")));
		BOOST_CHECK(registry.contains("scoped"));
	}
	BOOST_CHECK(!registry.contains("scoped"));
}

BOOST_AUTO_TEST_CASE(MovedRegistration_KeepsPanelUntilTheTargetDies)
{
	edt::panel_registry registry;
	auto reg = registry.add(desc_of(std::make_shared<mock_panel>("moved", "Moved")));
	{
		edt::registration target = std::move(reg);
		BOOST_CHECK(!reg.active());
		BOOST_CHECK(target.active());
		BOOST_CHECK(registry.contains("moved"));
	}
	BOOST_CHECK(!registry.contains("moved"));
}

BOOST_AUTO_TEST_CASE(RegistrationOutlivingRegistry_IsHarmless)
{
	edt::registration reg;
	{
		edt::panel_registry registry;
		reg = registry.add(desc_of(std::make_shared<mock_panel>("late", "Late")));
	}
	BOOST_CHECK_NO_THROW(reg.reset());
}

BOOST_AUTO_TEST_CASE(OpenByDefault_SetsInitialState)
{
	edt::panel_registry registry;
	auto shown = std::make_shared<mock_panel>("shown", "Shown");
	auto hidden = std::make_shared<mock_panel>("hidden", "Hidden");
	hidden->set_open(true);
	auto r1 = registry.add(desc_of(shown, edt::dock_slot::left, true));
	auto r2 = registry.add(desc_of(hidden, edt::dock_slot::left, false));
	BOOST_CHECK(registry.is_open("shown"));
	BOOST_CHECK(!registry.is_open("hidden"));
	BOOST_CHECK(!hidden->is_open());
}

BOOST_AUTO_TEST_CASE(SetOpen_TogglesThePanel)
{
	edt::panel_registry registry;
	auto panel = std::make_shared<mock_panel>("toggle_panel", "Toggle");
	auto reg = registry.add(desc_of(panel));

	BOOST_CHECK(registry.is_open("toggle_panel"));
	BOOST_CHECK(registry.set_open("toggle_panel", false));
	BOOST_CHECK(!panel->is_open());
	BOOST_CHECK(registry.set_open("toggle_panel", true));
	BOOST_CHECK(panel->is_open());
	BOOST_CHECK(!registry.set_open("ghost", true));
}

BOOST_AUTO_TEST_CASE(RegistrationOrder_IsKeptInSnapshotAndSlots)
{
	edt::panel_registry registry;
	auto a = registry.add(desc_of(std::make_shared<mock_panel>("a", "A"), edt::dock_slot::left));
	auto b = registry.add(desc_of(std::make_shared<mock_panel>("b", "B"), edt::dock_slot::center));
	auto c = registry.add(desc_of(std::make_shared<mock_panel>("c", "C"), edt::dock_slot::bottom_right, false));

	BOOST_TEST((ids_of(registry) == std::vector<std::string>{ "a", "b", "c" }));
	const auto infos = registry.panels();
	BOOST_CHECK(infos[0].slot == edt::dock_slot::left);
	BOOST_CHECK(infos[1].slot == edt::dock_slot::center);
	BOOST_CHECK(infos[2].slot == edt::dock_slot::bottom_right);
	BOOST_CHECK_EQUAL(infos[2].title, "C");
	BOOST_CHECK(infos[0].open && !infos[2].open);
}

BOOST_AUTO_TEST_CASE(RemovingAPanel_KeepsTheOrderOfTheOthers)
{
	edt::panel_registry registry;
	auto a = registry.add(desc_of(std::make_shared<mock_panel>("a", "A")));
	auto b = registry.add(desc_of(std::make_shared<mock_panel>("b", "B")));
	auto c = registry.add(desc_of(std::make_shared<mock_panel>("c", "C")));
	b.reset();
	BOOST_TEST((ids_of(registry) == std::vector<std::string>{ "a", "c" }));
	auto b2 = registry.add(desc_of(std::make_shared<mock_panel>("b", "B")));
	BOOST_TEST((ids_of(registry) == std::vector<std::string>{ "a", "c", "b" }));
}

BOOST_AUTO_TEST_CASE(DuplicateId_IsRefusedAndTheFirstPanelStays)
{
	edt::panel_registry registry;
	auto first = std::make_shared<mock_panel>("same", "First");
	auto second = std::make_shared<mock_panel>("same", "First");
	auto r1 = registry.add(desc_of(first));
	auto r2 = registry.add(desc_of(second));

	BOOST_CHECK(r1.active());
	BOOST_CHECK(!r2.active());
	BOOST_CHECK_EQUAL(registry.size(), 1u);
	BOOST_CHECK(registry.find("same") == first.get());
	r2.reset(); // a refused registration removes nothing
	BOOST_CHECK(registry.find("same") == first.get());
}

BOOST_AUTO_TEST_CASE(InvalidDescriptions_AreRefused)
{
	edt::panel_registry registry;
	auto panel = std::make_shared<mock_panel>("p", "P");

	auto wrong_id = desc_of(panel);
	wrong_id.id = "other";
	auto wrong_title = desc_of(panel);
	wrong_title.title = "Other";
	auto no_panel = desc_of(panel);
	no_panel.panel = nullptr;

	BOOST_CHECK(!registry.add(std::move(wrong_id)).active());
	BOOST_CHECK(!registry.add(std::move(wrong_title)).active());
	BOOST_CHECK(!registry.add(std::move(no_panel)).active());
	BOOST_CHECK(!registry.add(desc_of(std::make_shared<mock_panel>("", ""))).active());
	BOOST_CHECK_EQUAL(registry.size(), 0u);
}

BOOST_AUTO_TEST_CASE(RenderAll_VisitsPanelsInRegistrationOrder)
{
	edt::panel_registry registry;
	std::vector<std::string> log;
	auto a = registry.add(desc_of(std::make_shared<mock_panel>("a", "A", &log), edt::dock_slot::left, false));
	auto b = registry.add(desc_of(std::make_shared<mock_panel>("b", "B", &log), edt::dock_slot::left, false));
	auto c = registry.add(desc_of(std::make_shared<mock_panel>("c", "C", &log), edt::dock_slot::left, false));
	b.reset();
	registry.render_all();
	BOOST_TEST((log == std::vector<std::string>{ "a", "c" }));
}

BOOST_AUTO_TEST_SUITE_END()

// ─────────────────────────────────────────────
BOOST_AUTO_TEST_SUITE(SceneHierarchyPanelTests)

BOOST_AUTO_TEST_CASE(DefaultSelectedNode_IsNull)
{
	edt::scene_hierarchy_panel panel;
	BOOST_CHECK(panel.get_selected_node() == nullptr);
}

BOOST_AUTO_TEST_CASE(SetWorldDesc_Null_DoesNotCrash)
{
	edt::scene_hierarchy_panel panel;
	BOOST_CHECK_NO_THROW(panel.set_world_desc(nullptr));
}

BOOST_AUTO_TEST_CASE(SetWorldDesc_ResetsSelection)
{
	edt::scene_hierarchy_panel panel;
	scn::world_desc desc;

	// Set a desc, manually set a selected node pointer
	panel.set_world_desc(&desc);
	BOOST_CHECK(panel.get_selected_node() == nullptr);

	// Simulate a selected node, then set a new desc — selection must reset
	scn::prefab_desc::prefab_node node;
	node.name = "test";
	panel.set_selected_node(&node);
	BOOST_REQUIRE(panel.get_selected_node() == &node);

	panel.set_world_desc(&desc);
	BOOST_CHECK(panel.get_selected_node() == nullptr);
}

BOOST_AUTO_TEST_CASE(SetSelectedNode_TrackedCorrectly)
{
	edt::scene_hierarchy_panel panel;
	scn::prefab_desc::prefab_node node;
	node.name = "myNode";

	panel.set_selected_node(&node);
	BOOST_CHECK(panel.get_selected_node() == &node);
}

BOOST_AUTO_TEST_CASE(SetSelectedNode_ToNull)
{
	edt::scene_hierarchy_panel panel;
	scn::prefab_desc::prefab_node node;
	panel.set_selected_node(&node);
	panel.set_selected_node(nullptr);
	BOOST_CHECK(panel.get_selected_node() == nullptr);
}

BOOST_AUTO_TEST_CASE(OnNodeSelectedCallback_Stored)
{
	edt::scene_hierarchy_panel panel;
	bool called = false;
	panel.set_on_node_selected([&](scn::prefab_desc::prefab_node*) { called = true; });
	// Callback сохранён — проверяем только что установка не падает
	BOOST_CHECK(!called);
}

BOOST_AUTO_TEST_CASE(SetWorldNames_DoesNotCrash)
{
	edt::scene_hierarchy_panel panel;
	BOOST_CHECK_NO_THROW(panel.set_world_names({ "World1", "World2" }, 0));
}

BOOST_AUTO_TEST_SUITE_END()
