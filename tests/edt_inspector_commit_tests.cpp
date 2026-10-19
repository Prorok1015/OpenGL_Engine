#include <boost/test/unit_test.hpp>
#include "edt_inspector_panel.h"
#include <snk/editor/edt_inspector_registry.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/gui/gui_menu_layout.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
	// Headless ImGui context: enough to run Begin/End and item-activity queries without a GPU.
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

	scn::prefab_desc::prefab_node make_node_with_camera()
	{
		scn::prefab_desc::prefab_node node;
		node.name = "n";
		scn::prefab_desc::prefab_comp_node comp;
		comp.type_name = std::string(scn::camera_desc::__type);
		comp.overrides["fov"] = 60.0; // renderers are only invoked for components with overrides
		node.components[comp.type_name] = comp;
		return node;
	}
}

BOOST_AUTO_TEST_SUITE(InspectorCommitTests)

BOOST_AUTO_TEST_CASE(RendererChange_NoActiveItem_CommitsOnce)
{
	imgui_ctx imgui;
	edt::inspector_registry registry;
	auto reg = registry.add<scn::camera_desc>([](scn::prefab_desc::prefab_comp_node&) { return true; });

	auto node = make_node_with_camera();
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 1);
}

BOOST_AUTO_TEST_CASE(RendererChange_WhileItemActive_CommitsOnceOnRelease)
{
	imgui_ctx imgui;
	edt::inspector_registry registry;
	bool hold = true;
	auto reg = registry.add<scn::camera_desc>([&](scn::prefab_desc::prefab_comp_node&) {
		if (hold) {
			// Simulates a drag in progress: item is active and reports "changed" every frame.
			ImGuiID id = ImGui::GetID("drag");
			ImGui::SetActiveID(id, ImGui::GetCurrentWindow());
			ImGui::KeepAliveID(id);
			return true;
		}
		return false;
	});

	auto node = make_node_with_camera();
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	for (int i = 0; i < 5; ++i)
		imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 0);

	hold = false;
	ImGui::ClearActiveID();
	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 1);

	imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 1);
}

BOOST_AUTO_TEST_CASE(NoChange_NeverCommits)
{
	imgui_ctx imgui;
	edt::inspector_registry registry;
	auto reg = registry.add<scn::camera_desc>([](scn::prefab_desc::prefab_comp_node&) { return false; });

	auto node = make_node_with_camera();
	edt::inspector_panel panel;
	int commits = 0;
	panel.set_selected_node(&node);
	panel.set_inspector_registry(&registry);
	panel.set_on_node_changed([&] { ++commits; });

	for (int i = 0; i < 3; ++i)
		imgui.frame([&] { panel.render(); });
	BOOST_CHECK_EQUAL(commits, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(MenuLayoutImplicitTests)

BOOST_AUTO_TEST_CASE(UnregisterImplicit_AbsentId_KeepsOthers)
{
	gui::menu_layout_manager mgr;
	int calls = 0;
	mgr.register_implicit("keep", [&] { ++calls; return true; });

	BOOST_CHECK_NO_THROW(mgr.unregister_implicit("missing"));
	mgr.process();
	BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_CASE(UnregisterImplicit_EmptyList_DoesNotCrash)
{
	gui::menu_layout_manager mgr;
	BOOST_CHECK_NO_THROW(mgr.unregister_implicit("missing"));
}

BOOST_AUTO_TEST_CASE(UnregisterImplicit_Present_Removes)
{
	gui::menu_layout_manager mgr;
	int a = 0, b = 0;
	mgr.register_implicit("a", [&] { ++a; return true; });
	mgr.register_implicit("b", [&] { ++b; return true; });
	mgr.unregister_implicit("a");
	mgr.process();
	BOOST_CHECK_EQUAL(a, 0);
	BOOST_CHECK_EQUAL(b, 1);
}

BOOST_AUTO_TEST_CASE(RegisterImplicit_Duplicate_KeepsFirst)
{
	gui::menu_layout_manager mgr;
	int first = 0, second = 0;
	mgr.register_implicit("x", [&] { ++first; return true; });
	mgr.register_implicit("x", [&] { ++second; return true; });
	mgr.process();
	BOOST_CHECK_EQUAL(first, 1);
	BOOST_CHECK_EQUAL(second, 0);
}

BOOST_AUTO_TEST_CASE(Process_CallbackReturnsFalse_RemovedOnlyThatOne)
{
	gui::menu_layout_manager mgr;
	int once = 0, always = 0;
	mgr.register_implicit("once", [&] { ++once; return false; });
	mgr.register_implicit("always", [&] { ++always; return true; });
	mgr.process();
	mgr.process();
	BOOST_CHECK_EQUAL(once, 1);
	BOOST_CHECK_EQUAL(always, 2);
}

BOOST_AUTO_TEST_SUITE_END()
