#include <boost/test/unit_test.hpp>
#include <TextEditor.h>
#include <imgui.h>
#include <string>

// The vendored text widget of the GLSL editor (#1202): it must build against the pinned Dear ImGui and run a frame of an
// ImGui context without a renderer. The adapter that drives it from the theme and the document comes in later PRs.
namespace {

struct imgui_frame_env
{
	imgui_frame_env()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* pixels = nullptr;
		int width = 0;
		int height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}

	~imgui_frame_env() { ImGui::DestroyContext(ctx); }

	// One frame with the editor in a window.
	void frame(TextEditor& editor)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(600.f, 400.f));
		if (ImGui::Begin("code")) editor.Render("##code");
		ImGui::End();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

constexpr const char* k_shader = "#version 450 core\n// a comment\nuniform float u_time;\nvoid main()\n{\n\tgl_Position = vec4(u_time);\n}\n";

}

BOOST_FIXTURE_TEST_SUITE(ColorTextEdit, imgui_frame_env)

BOOST_AUTO_TEST_CASE(SetText_ThenGetText_GivesTheSameText_AndCountsTheLines)
{
	TextEditor editor;
	editor.SetLanguage(TextEditor::Language::Glsl());
	editor.SetText(k_shader);

	BOOST_TEST(editor.GetText().find("gl_Position") != std::string::npos);
	BOOST_TEST(editor.GetLineCount() >= 7);
	BOOST_TEST(!editor.IsEmpty());
}

BOOST_AUTO_TEST_CASE(ARenderedFrame_WithMarkersAndAColourPalette_Runs)
{
	TextEditor editor;
	editor.SetLanguage(TextEditor::Language::Glsl());
	editor.SetText(k_shader);
	editor.AddMarker(5, IM_COL32(255, 80, 80, 255), IM_COL32(255, 80, 80, 60), "error", "undeclared identifier");

	BOOST_TEST(editor.HasMarkers());
	frame(editor);
	frame(editor);

	BOOST_TEST(editor.GetText() == k_shader);
}

BOOST_AUTO_TEST_CASE(AReadOnlyEditor_CannotUndo_AndKeepsItsText)
{
	TextEditor editor;
	editor.SetText(k_shader);
	editor.SetReadOnlyEnabled(true);

	BOOST_TEST(editor.IsReadOnlyEnabled());
	BOOST_TEST(!editor.CanUndo());
	frame(editor);
	BOOST_TEST(editor.GetText() == k_shader);
}

BOOST_AUTO_TEST_SUITE_END()
