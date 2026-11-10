#include <boost/test/unit_test.hpp>
#include "edt_problems_panel.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <utility>
#include <vector>

// #596: a double click, Enter or "Go to" on a row of Problems hands the row and its source to the editor.
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

edt::problem row_with(edt::problem_severity severity, std::string message)
{
	edt::problem item;
	item.severity = severity;
	item.file = "levels/l.desc";
	item.message = std::move(message);
	return item;
}

struct panel_fixture
{
	panel_fixture()
	{
		problems.set("build", { row_with(edt::problem_severity::error, "from build") });
		problems.set("desc", { row_with(edt::problem_severity::warning, "from desc") });
		panel.set_on_activate([this](const std::string& source, const edt::problem& item) {
			activated.emplace_back(source, item.message);
		});
		frame();
	}

	void frame()
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(800.f, 400.f));
		panel.render();
		ImGui::EndFrame();
	}

	void move_to(float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame();
	}

	void press(ImGuiMouseButton button)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(button, true);
		frame();
		io.AddMouseButtonEvent(button, false);
		frame();
	}

	void key(ImGuiKey pressed)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddKeyEvent(pressed, true);
		frame();
		io.AddKeyEvent(pressed, false);
		frame();
	}

	// The first item the mouse hovers going from `from` toward `to` along x = `x`.
	bool hover_first(float x, float from, float to)
	{
		const float step = from < to ? 2.f : -2.f;
		for (float y = from; step > 0 ? y < to : y > to; y += step) {
			move_to(x, y);
			if (imgui.ctx->HoveredId != 0) return true;
		}
		return false;
	}

	// The lowest row: the first item going up from the bottom (clear of the window's resize border).
	bool hover_last_row() { return hover_first(200.f, 380.f, 0.f); }

	imgui_ctx imgui;
	edt::editor_events events;
	edt::problem_list problems{ &events };
	edt::problems_panel panel{ problems, {} };
	std::vector<std::pair<std::string, std::string>> activated;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtProblemsPanel, panel_fixture)

BOOST_AUTO_TEST_CASE(ADoubleClickOnARow_HandsItOverWithItsSource)
{
	BOOST_REQUIRE(hover_last_row());
	press(ImGuiMouseButton_Left);
	BOOST_TEST(activated.empty()); // one click selects, it does not go
	press(ImGuiMouseButton_Left);

	BOOST_REQUIRE_EQUAL(activated.size(), 1u);
	const auto& [source, message] = activated.front();
	BOOST_TEST(message == "from " + source);
}

BOOST_AUTO_TEST_CASE(EnterOnTheFocusedRow_HandsItOver)
{
	BOOST_REQUIRE(hover_last_row());
	press(ImGuiMouseButton_Left);
	BOOST_REQUIRE(activated.empty());
	key(ImGuiKey_Enter);
	key(ImGuiKey_KeypadEnter);

	BOOST_REQUIRE_EQUAL(activated.size(), 2u);
	BOOST_TEST((activated[0] == activated[1]));
	BOOST_TEST(activated[0].second == "from " + activated[0].first);
}

BOOST_AUTO_TEST_CASE(GoToInTheRowsMenu_HandsItOver)
{
	BOOST_REQUIRE(hover_last_row());
	press(ImGuiMouseButton_Right);
	BOOST_REQUIRE(!imgui.ctx->OpenPopupStack.empty());
	const ImGuiWindow* menu = imgui.ctx->OpenPopupStack.back().Window;
	BOOST_REQUIRE(menu);
	// "Go to" is the menu's first item.
	move_to(menu->Pos.x + menu->WindowPadding.x + 10.f, menu->Pos.y + menu->WindowPadding.y + 4.f);
	press(ImGuiMouseButton_Left);

	BOOST_REQUIRE_EQUAL(activated.size(), 1u);
	BOOST_TEST(activated[0].second == "from " + activated[0].first);
}

BOOST_AUTO_TEST_CASE(WithASeverityFilteredOut_TheVisibleRowIsTheOneHandedOver)
{
	// The first checkbox, "Errors", hides the row of "build".
	BOOST_REQUIRE(hover_first(40.f, 2.f, 120.f));
	press(ImGuiMouseButton_Left);
	BOOST_REQUIRE(hover_last_row());
	press(ImGuiMouseButton_Left);
	press(ImGuiMouseButton_Left);

	BOOST_REQUIRE_EQUAL(activated.size(), 1u);
	BOOST_TEST(activated[0].first == "desc");
	BOOST_TEST(activated[0].second == "from desc");
}

BOOST_AUTO_TEST_CASE(WithoutACallback_ADoubleClickChangesNothing)
{
	panel.set_on_activate({});
	BOOST_REQUIRE(hover_last_row());
	const auto generation = problems.generation();
	press(ImGuiMouseButton_Left);
	press(ImGuiMouseButton_Left);
	BOOST_TEST(problems.generation() == generation);
	BOOST_TEST(problems.list().size() == 2u);
	BOOST_TEST(activated.empty());
}

BOOST_AUTO_TEST_SUITE_END()
