#include <boost/test/unit_test.hpp>
#include "edt_problems_panel.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <utility>
#include <vector>

// #596: a double click on a row of Problems hands the row and its source to the editor.
namespace {

struct imgui_ctx
{
	imgui_ctx()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 400.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~imgui_ctx() { ImGui::DestroyContext(ctx); }

	ImGuiContext* ctx = nullptr;
};

edt::problem row_with(std::string message)
{
	edt::problem item;
	item.severity = edt::problem_severity::warning;
	item.file = "levels/l.desc";
	item.message = std::move(message);
	return item;
}

struct panel_fixture
{
	panel_fixture()
	{
		problems.set("build", { row_with("from build") });
		problems.set("desc", { row_with("from desc") });
	}

	void frame()
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(800.f, 400.f));
		panel.render();
		ImGui::EndFrame();
	}

	// The lowest row: the first item the mouse hovers going up from the bottom (clear of the window's resize border).
	bool hover_last_row()
	{
		ImGuiIO& io = ImGui::GetIO();
		for (float y = 380.f; y > 0.f; y -= 2.f) {
			io.AddMousePosEvent(200.f, y);
			frame();
			if (imgui.ctx->HoveredId != 0) return true;
		}
		return false;
	}

	void double_click()
	{
		ImGuiIO& io = ImGui::GetIO();
		for (int i = 0; i < 2; ++i) {
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			frame();
		}
	}

	imgui_ctx imgui;
	edt::editor_events events;
	edt::problem_list problems{ &events };
	edt::problems_panel panel{ problems, {} };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtProblemsPanel, panel_fixture)

BOOST_AUTO_TEST_CASE(ADoubleClickOnARow_HandsItOverWithItsSource)
{
	std::vector<std::pair<std::string, std::string>> activated;
	panel.set_on_activate([&activated](const std::string& source, const edt::problem& item) {
		activated.emplace_back(source, item.message);
	});
	frame();
	BOOST_REQUIRE(hover_last_row());
	double_click();

	BOOST_REQUIRE_EQUAL(activated.size(), 1u);
	const auto& [source, message] = activated.front();
	BOOST_TEST(message == "from " + source);
}

BOOST_AUTO_TEST_CASE(ASingleClick_HandsNothingOver)
{
	int activated = 0;
	panel.set_on_activate([&activated](const std::string&, const edt::problem&) { ++activated; });
	frame();
	BOOST_REQUIRE(hover_last_row());
	ImGuiIO& io = ImGui::GetIO();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
	BOOST_TEST(activated == 0);
}

BOOST_AUTO_TEST_CASE(WithoutACallback_ADoubleClickChangesNothing)
{
	frame();
	BOOST_REQUIRE(hover_last_row());
	const auto generation = problems.generation();
	double_click();
	BOOST_TEST(problems.generation() == generation);
	BOOST_TEST(problems.list().size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
