#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_shader_editor_panel.h"
#include "edt_shader_palette.h"
#include "edt_shader_source_document.h"
#include "edt_shader_text_view.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The window of the GLSL editor (#1202): the theme's colours in the text widget's palette, the widget behind its thin adapter, and the
// panel that keeps a shader_source_document, the Problems list and the widget in step.
namespace {

namespace fs = std::filesystem;

constexpr const char* k_frag = "#version 450 core\nout vec4 colour;\nvoid main()\n{\n\tcolour = vec4(1.0);\n}\n";

struct imgui_env
{
	imgui_env()
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

	~imgui_env() { ImGui::DestroyContext(ctx); }

	template <class Draw>
	void frame(Draw&& draw)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(600.f, 400.f));
		draw();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

edt::sourced_problem shader_problem(int line, std::string file = "res://shaders/scene.frag")
{
	edt::sourced_problem row;
	row.source = "shader";
	row.item.file = std::move(file);
	row.item.line = line;
	row.item.code = "C0000";
	row.item.message = "syntax error";
	return row;
}

struct panel_fixture : imgui_env, test_util::scene_env
{
	panel_fixture()
	{
		fs::create_directories(res_root / "shaders");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		std::ofstream(res_root / "shaders/scene.frag", std::ios::binary) << k_frag;
		document = std::make_unique<edt::shader_source_document>(services);
		document->assign_id(21);
		edt::desc_file_layout layout;
		layout.top_layer = [this] { return res_root; };
		layout.layers = [this] { return std::vector<fs::path>{ res_root }; };
		layout.open_level_files = [] { return std::vector<std::string>{}; };
		layout.write_blocker = [] { return std::optional<std::string>{}; };
		document->set_files(std::move(layout));
		document->bind_events(&events);
		document->open(res::tag{ "res://shaders/scene.frag" });
		panel.set_problems(&problems);
		panel.set_context(document.get());
	}

	~panel_fixture()
	{
		panel.set_context(nullptr);
		document.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	void draw() { frame([this] { panel.render(); }); }

	test_util::temp_dir dir{ "edt_shader_editor_view" };
	const fs::path res_root = dir.path() / "res";
	ecs::system_factory sf;
	edt::editor_services services;
	edt::editor_events events;
	edt::problem_list problems;
	std::unique_ptr<edt::shader_source_document> document;
	edt::shader_editor_panel panel;
};

}

BOOST_AUTO_TEST_SUITE(EdtShaderPalette)

BOOST_AUTO_TEST_CASE(TheCodeColoursComeFromTheSemanticColours_TheTextAndTheBackgroundFromTheStyle)
{
	edt::shader_palette_source source;
	source.semantic.info = ImVec4(0.1f, 0.2f, 0.3f, 1.f);
	source.semantic.text_secondary = ImVec4(0.4f, 0.5f, 0.6f, 1.f);
	source.semantic.error = ImVec4(0.7f, 0.1f, 0.1f, 1.f);
	source.text = ImVec4(0.9f, 0.9f, 0.9f, 1.f);
	source.background = ImVec4(0.1f, 0.1f, 0.1f, 1.f);
	source.selection = ImVec4(0.2f, 0.3f, 0.5f, 1.f);

	imgui_env env;
	const TextEditor::Palette palette = edt::make_shader_palette(source);
	const auto packed = [](const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); };
	BOOST_TEST(palette.get(TextEditor::Color::keyword) == packed(source.semantic.info));
	BOOST_TEST(palette.get(TextEditor::Color::comment) == packed(source.semantic.text_secondary));
	BOOST_TEST(palette.get(TextEditor::Color::matchingBracketError) == packed(source.semantic.error));
	BOOST_TEST(palette.get(TextEditor::Color::text) == packed(source.text));
	BOOST_TEST(palette.get(TextEditor::Color::background) == packed(source.background));
	BOOST_TEST(palette.get(TextEditor::Color::selection) == packed(source.selection));
}

BOOST_AUTO_TEST_CASE(AnotherTheme_GivesAnotherPalette)
{
	edt::shader_palette_source dark;
	edt::shader_palette_source light = dark;
	light.semantic.info = ImVec4(0.9f, 0.1f, 0.1f, 1.f);

	imgui_env env;
	BOOST_TEST((edt::make_shader_palette(dark).get(TextEditor::Color::keyword) != edt::make_shader_palette(light).get(TextEditor::Color::keyword)));
}

BOOST_AUTO_TEST_CASE(MixColors_GoesFromTheFirstColourToTheSecond)
{
	const ImVec4 a(0.f, 0.f, 0.f, 1.f);
	const ImVec4 b(1.f, 0.5f, 0.25f, 1.f);
	BOOST_TEST(edt::mix_colors(a, b, 0.f).x == 0.f);
	BOOST_TEST(edt::mix_colors(a, b, 1.f).y == 0.5f);
	BOOST_TEST(edt::mix_colors(a, b, 0.5f).z == 0.125f);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtShaderTextView, imgui_env)

BOOST_AUTO_TEST_CASE(Load_ThenText_GivesTheSameText_AndRenderReportsNoEdit)
{
	edt::shader_text_view view;
	view.load(k_frag);
	BOOST_TEST(view.text().find("vec4(1.0)") != std::string::npos);
	BOOST_TEST(view.line_count() >= 6);

	bool edited = false;
	frame([&] { if (ImGui::Begin("code")) edited = view.render("##code", ImVec2(500.f, 300.f)); ImGui::End(); });
	BOOST_TEST(!edited);
}

BOOST_AUTO_TEST_CASE(AnInsertedText_IsReportedOnceByTheNextRender)
{
	edt::shader_text_view view;
	view.load("void main() {}\n");
	view.insert_text("// x\n");

	bool first = false;
	bool second = false;
	frame([&] { if (ImGui::Begin("code")) first = view.render("##code", ImVec2(500.f, 300.f)); ImGui::End(); });
	frame([&] { if (ImGui::Begin("code")) second = view.render("##code", ImVec2(500.f, 300.f)); ImGui::End(); });
	BOOST_TEST(first);
	BOOST_TEST(view.text().rfind("// x\n", 0) == 0u);
	BOOST_TEST(!second);
}

BOOST_AUTO_TEST_CASE(Load_AfterAnEdit_IsNotReportedAsAnEdit)
{
	edt::shader_text_view view;
	view.load("a\n");
	view.insert_text("b");
	view.load("c\n");

	bool edited = false;
	frame([&] { if (ImGui::Begin("code")) edited = view.render("##code", ImVec2(500.f, 300.f)); ImGui::End(); });
	BOOST_TEST(!edited);
}

BOOST_AUTO_TEST_CASE(Markers_AreReplacedAsAWhole_ByLineAndSeverity)
{
	edt::shader_text_view view;
	view.load(k_frag);
	gui::theme_semantic_colors colors;

	edt::shader_marker_set markers;
	markers.markers.push_back({ 3, edt::problem_severity::error, "[C0000] syntax error" });
	view.set_markers(markers, colors);
	BOOST_TEST(view.has_markers());
	frame([&] { if (ImGui::Begin("code")) view.render("##code", ImVec2(500.f, 300.f)); ImGui::End(); });

	view.set_markers(edt::shader_marker_set{}, colors);
	BOOST_TEST(!view.has_markers());
}

BOOST_AUTO_TEST_CASE(ReadOnly_IsKept)
{
	edt::shader_text_view view;
	view.set_read_only(true);
	BOOST_TEST(view.read_only());
	view.set_read_only(false);
	BOOST_TEST(!view.read_only());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtShaderEditorPanel, panel_fixture)

BOOST_AUTO_TEST_CASE(AFrame_ShowsTheDocumentsText_AndALaterFrameChangesNothing)
{
	BOOST_REQUIRE(document->is_open());
	const std::uint64_t generation = document->generation();
	draw();
	draw();
	BOOST_TEST(document->generation() == generation);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(WithoutADocument_TheFrameRuns)
{
	panel.set_context(nullptr);
	draw();
	BOOST_TEST((panel.context() == nullptr));
}

BOOST_AUTO_TEST_CASE(AProblemOfTheFile_BecomesAMarker_AProblemOfAnotherFileDoesNot)
{
	problems.set("shader", { shader_problem(4, "res://shaders/other.frag").item });
	draw();
	BOOST_TEST(!panel.has_markers());

	problems.set("shader", { shader_problem(3).item });
	draw();
	BOOST_TEST(panel.has_markers());
	BOOST_TEST(panel.unplaced().empty());

	problems.clear("shader");
	draw();
	BOOST_TEST(!panel.has_markers());
}

BOOST_AUTO_TEST_CASE(AProblemWithoutALine_IsShownInTheBannerNotInTheText)
{
	problems.set("shader", { shader_problem(0).item });
	draw();
	BOOST_TEST(!panel.has_markers());
	BOOST_TEST(panel.unplaced().size() == 1u);
}

BOOST_AUTO_TEST_CASE(ADocumentTextSetFromOutside_IsTakenByTheWidgetOnTheNextFrame)
{
	draw();
	BOOST_REQUIRE(document->set_text("void main() {}\n"));
	draw();
	BOOST_TEST(document->text() == "void main() {}\n");
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(TheDocumentSeesTheTypingNotHandedOver_AsDirty_AndSaveHandsItOverFirst)
{
	draw();
	panel.insert_text("// z\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());
	BOOST_TEST(document->is_dirty());

	const edt::save_result saved = document->save();

	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(!panel.has_pending_edits());
	std::ifstream in(res_root / "shaders/scene.frag", std::ios::binary);
	const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	BOOST_TEST(written.rfind("// z\n", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(AWindowThatLetsGo_LeavesTheDocumentWithoutAHookIntoIt)
{
	draw();
	panel.insert_text("// z\n");
	draw();
	panel.set_context(nullptr);

	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->save().saved);
}

BOOST_AUTO_TEST_CASE(TypedText_GoesToTheDocumentAfterAPause_NotAtOnce)
{
	draw();
	panel.insert_text("// x\n");
	draw();
	BOOST_TEST(panel.has_pending_edits());
	BOOST_TEST(document->text() == k_frag);

	ImGui::GetIO().DeltaTime = 0.5f;
	draw();
	BOOST_TEST(!panel.has_pending_edits());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->text().rfind("// x\n", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(FlushEdits_GivesTheDocumentTheTextTypedSoFar)
{
	draw();
	panel.insert_text("// y\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());

	panel.flush_edits();

	BOOST_TEST(!panel.has_pending_edits());
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(PressClose_FlushesTheTypedTextFirst)
{
	draw();
	panel.insert_text("// z\n");
	draw();
	bool dirty_when_closed = false;
	panel.set_on_close([&] { dirty_when_closed = document->is_dirty(); });

	panel.press_close();

	BOOST_TEST(dirty_when_closed);
}

BOOST_AUTO_TEST_CASE(Revert_TakesTheFilesText_AndTheTypingNotHandedOverGoesWithIt)
{
	draw();
	panel.insert_text("// typed\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());
	BOOST_TEST(panel.is_dirty());

	panel.request_revert();
	draw();

	BOOST_TEST(panel.error().empty());
	BOOST_TEST(!panel.has_pending_edits());
	BOOST_TEST(!panel.is_dirty());
	BOOST_TEST(document->text() == k_frag);
	BOOST_TEST(panel.widget_text() == document->text());
}

BOOST_AUTO_TEST_CASE(Revert_ThatFails_KeepsTheTyping)
{
	draw();
	panel.insert_text("// typed\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());
	fs::remove(res_root / "shaders/scene.frag");

	panel.request_revert();

	BOOST_TEST(!panel.error().empty());
	BOOST_TEST(panel.has_pending_edits());
}

BOOST_AUTO_TEST_CASE(Overwrite_HandsTheTypingOverFirst_ThenWritesIt)
{
	draw();
	panel.insert_text("// typed\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());

	panel.request_overwrite();

	BOOST_TEST(panel.error().empty());
	BOOST_TEST(!panel.has_pending_edits());
	BOOST_TEST(!document->is_dirty());
	std::ifstream in(res_root / "shaders/scene.frag", std::ios::binary);
	const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	BOOST_TEST(written.rfind("// typed\n", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(AnOutsideChangeWhileTyping_DoesNotPartTheWidgetFromTheDocument_AndRaisesTheConflict)
{
	draw();
	panel.insert_text("// typed\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());
	BOOST_REQUIRE(document->text() == k_frag);

	std::ofstream(res_root / "shaders/scene.frag", std::ios::binary | std::ios::trunc) << "// from disk\n";
	rs->signal_changed(res::tag{ "res://shaders/scene.frag" });
	pump();
	BOOST_REQUIRE(document->text() == "// from disk\n");
	draw();

	BOOST_TEST(panel.widget_text() == document->text());
	BOOST_TEST(document->text().rfind("// typed\n", 0) == 0u);
	BOOST_TEST(document->changed_on_disk());
	BOOST_TEST(!panel.has_pending_edits());

	panel.request_revert();
	draw();
	BOOST_TEST(document->text() == "// from disk\n");
	BOOST_TEST(panel.widget_text() == document->text());
	BOOST_TEST(!document->changed_on_disk());
}

BOOST_AUTO_TEST_CASE(TypingThatCannotBeHandedOver_StaysInTheWidget_AndGoesInWhenThePlayStops)
{
	bool locked = false;
	document->set_lock_source([&locked] { return locked; });
	draw();
	panel.insert_text("// typed\n");
	draw();
	BOOST_REQUIRE(panel.has_pending_edits());

	locked = true;
	ImGui::GetIO().DeltaTime = 0.5f;
	draw();
	BOOST_TEST(panel.has_pending_edits());
	BOOST_TEST(panel.edits_held_back());
	BOOST_TEST(document->text() == k_frag);
	BOOST_TEST(panel.is_dirty());

	locked = false;
	draw();
	BOOST_TEST(!panel.has_pending_edits());
	BOOST_TEST(!panel.edits_held_back());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(panel.widget_text() == document->text());
}

BOOST_AUTO_TEST_CASE(Typing_ClearsTheMarkersUntilTheNextProblemsUpdate)
{
	problems.set("shader", { shader_problem(3).item });
	draw();
	BOOST_REQUIRE(panel.has_markers());

	panel.insert_text("// typed\n");
	draw();
	draw();
	BOOST_TEST(!panel.has_markers());

	problems.set("shader", { shader_problem(4).item });
	draw();
	BOOST_TEST(panel.has_markers());
}

BOOST_AUTO_TEST_CASE(ADocumentOfOneMegabyte_OpensEditsAndHandsOver)
{
	std::string big = "#version 450 core\n";
	big.reserve(1024 * 1024 + 64);
	while (big.size() < 1024 * 1024) big += "// a line of a long shader source that is only here for its size\n";
	std::ofstream(res_root / "shaders/big.frag", std::ios::binary) << big;
	document->open(res::tag{ "res://shaders/big.frag" });
	BOOST_REQUIRE_MESSAGE(document->is_open(), document->last_error());
	panel.set_context(nullptr);
	panel.set_context(document.get());

	draw();
	panel.insert_text("// typed\n");
	draw();
	draw();
	panel.flush_edits();

	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->text().size() == big.size() + 9);
}

BOOST_AUTO_TEST_CASE(TheClosedWindow_ReportsItOnce)
{
	int closed = 0;
	panel.set_on_close([&] { ++closed; });
	panel.set_open(false);
	draw();
	draw();
	draw();
	BOOST_TEST(closed <= 1);
}

BOOST_AUTO_TEST_CASE(Close_RunsTheCallback)
{
	int closed = 0;
	panel.set_on_close([&] { ++closed; });
	panel.press_close();
	BOOST_TEST(closed == 1);
}

BOOST_AUTO_TEST_SUITE_END()
