#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/gui/gui_gui_service_init.h>
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_theme_desc.h>
#include <snk/gui/gui_theme_service.h>
#include <snk/gui/gui_ui_font.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Theme presets (#1109): the shipped engine://themes/*.desc load through the resource and desc systems, resolve into
// a style, and anything a preset gets wrong ends in a fallback, never in a stop.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

namespace fs = std::filesystem;

float luminance(const ImVec4& c)
{
	return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool same_colors(const ImGuiStyle& a, const ImGuiStyle& b)
{
	return std::memcmp(a.Colors, b.Colors, sizeof(a.Colors)) == 0;
}

std::vector<std::byte> read_bytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> raw{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::vector<std::byte> out(raw.size());
	std::memcpy(out.data(), raw.data(), raw.size());
	return out;
}

// core/res served as engine:// and res:// from RAM, as the editor mounts it.
struct theme_env : test_util::ram_res_env
{
	desc::desc_system descs{ *rs };
	std::vector<gui::theme_service::theme> applied;
	// memory:// holds the test's own presets; the editor reads engine:// only (the default).
	gui::theme_service service{ *rs, res::tag{ "engine://themes/classic.desc" },
		[this](gui::theme_service::theme theme) { applied.push_back(std::move(theme)); },
		{ "engine://", "memory://" }, { "engine://", "memory://" } };
	gui::theme_service editor_service{ *rs, res::tag{ "engine://themes/classic.desc" },
		[this](gui::theme_service::theme theme) { applied.push_back(std::move(theme)); } };

	theme_env()
	{
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs),
			std::ref(descs));
		engine::gui::register_gui_descs(descs);
		rs->unregistrate_resolver(res::tag::default_protocol());
		auto& ram_res = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
		auto& ram_engine = rs->registrate_resolver<test_util::ram_resolver>(res::tag::engine);
		for (const std::string_view folder : { "themes", "fonts" }) {
			for (const auto& entry : fs::directory_iterator(fs::path{ SNK_CORE_RES_DIR } / folder)) {
				const std::string relative = std::string{ folder } + "/" + entry.path().filename().generic_string();
				const std::vector<std::byte> bytes = read_bytes(entry.path());
				ram_engine.files[res::tag{ res::tag::engine, relative }] = bytes;
				ram_res.files[res::tag{ res::tag::default_protocol(), relative }] = bytes;
			}
		}
	}

	~theme_env()
	{
		engine::gui::unregister_gui_descs(descs);
		rs->unregistrate_adapter(core::desc::desc_adapter_t::INFO);
	}

	void settle(gui::theme_service& themes)
	{
		for (int i = 0; i < 50 && themes.loading(); ++i) {
			pump();
			themes.poll();
		}
	}

	const gui::theme_service::theme& select(std::string_view preset) { return select(service, preset); }

	const gui::theme_service::theme& select(gui::theme_service& themes, std::string_view preset)
	{
		applied.clear();
		themes.select(res::tag{ preset });
		settle(themes);
		BOOST_REQUIRE(!themes.loading());
		BOOST_REQUIRE_EQUAL(applied.size(), 1u);
		return applied.front();
	}
};

constexpr std::string_view palette_presets[] = {
	"engine://themes/dark_neutral.desc",
	"engine://themes/dark_contrast.desc",
	"engine://themes/light.desc",
};

}

BOOST_FIXTURE_TEST_SUITE(GuiThemeTests, theme_env)

BOOST_AUTO_TEST_CASE(TheDefaultStyle_IsImGuisDarkStyle)
{
	ImGuiStyle dark;
	ImGui::StyleColorsDark(&dark);
	ImGuiStyle style;
	style.WindowRounding = 9.f;

	gui::apply_theme(gui::theme_style{}, style);

	BOOST_TEST(same_colors(style, dark));
	BOOST_TEST(style.WindowRounding == ImGuiStyle().WindowRounding);
	BOOST_TEST(gui::theme_style{}.font_px == 13.f);
}

BOOST_AUTO_TEST_CASE(TheClassicPreset_IsTheLookOfTheSmokeReferences)
{
	const auto& theme = select("engine://themes/classic.desc");

	BOOST_TEST(theme.style.imgui_dark);
	BOOST_TEST(theme.style.font_px == 13.f);
	BOOST_TEST(gui::ui_font::looks_like_font(theme.text_font));
	BOOST_TEST(theme.icon_font.empty());
}

BOOST_AUTO_TEST_CASE(EveryPalettePreset_LoadsWithItsFontsAndKeepsTextReadable)
{
	ImGuiStyle classic;
	gui::apply_theme(gui::theme_style{}, classic);
	for (const std::string_view preset : palette_presets) {
		BOOST_TEST_CONTEXT(preset) {
			const auto& theme = select(preset);
			BOOST_TEST(!theme.style.imgui_dark);
			BOOST_TEST(theme.style.font_px == 14.f);
			// The palette presets share their fonts: after the first, only the style changes.
			if (theme.fonts_changed) {
				BOOST_TEST(gui::ui_font::looks_like_font(theme.text_font));
				BOOST_TEST(gui::ui_font::looks_like_font(theme.icon_font));
			}
			BOOST_TEST(service.selected().view() == preset);

			ImGuiStyle style;
			gui::apply_theme(theme.style, style);
			BOOST_TEST(!same_colors(style, classic));
			const float text = luminance(style.Colors[ImGuiCol_Text]);
			BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_WindowBg])) > 0.6f);
			BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_FrameBg])) > 0.5f);
			BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_Header])) > 0.4f);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheLightPreset_HasDarkTextOnLightPanels)
{
	const auto& theme = select("engine://themes/light.desc");
	ImGuiStyle style;
	gui::apply_theme(theme.style, style);

	BOOST_TEST(theme.style.palette.light);
	BOOST_TEST(luminance(style.Colors[ImGuiCol_WindowBg]) > 0.8f);
	BOOST_TEST(luminance(style.Colors[ImGuiCol_Text]) < 0.2f);
}

BOOST_AUTO_TEST_CASE(AMissingPreset_FallsBackToTheFallbackPreset)
{
	const auto& theme = select("engine://themes/no_such_theme.desc");

	BOOST_TEST(theme.style.imgui_dark);
	BOOST_TEST(service.selected().view() == "engine://themes/classic.desc");
}

BOOST_AUTO_TEST_CASE(APresetWithAValueOutOfRange_FallsBackToTheFallbackPreset)
{
	const res::tag preset = put("huge.desc", R"({ "__type" : "gui.theme", "__version" : 1, "font_size" : 400.0 })");

	const auto& theme = select(preset.view());

	BOOST_TEST(theme.style.imgui_dark);
	BOOST_TEST(service.selected().view() == "engine://themes/classic.desc");
}

BOOST_AUTO_TEST_CASE(AFontFileThatIsNotAFont_IsReplacedByTheUiFont)
{
	put("garbage.ttf", "this is not a font, only bytes with a .ttf name");
	const res::tag preset = put("garbage_font.desc",
		R"({ "__type" : "gui.theme", "__version" : 1, "font" : "memory://t/garbage.ttf" })");

	const auto& theme = select(preset.view());

	BOOST_TEST(!theme.style.imgui_dark);
	BOOST_TEST(gui::ui_font::looks_like_font(theme.text_font));
	BOOST_TEST(service.selected().view() == preset.view());
}

BOOST_AUTO_TEST_CASE(AFontFileThatDoesNotExist_IsReplacedByTheUiFont)
{
	const res::tag preset = put("missing_font.desc",
		R"({ "__type" : "gui.theme", "__version" : 1, "font" : "memory://t/none.ttf" })");

	const auto& theme = select(preset.view());

	BOOST_TEST(!theme.style.imgui_dark);
	BOOST_TEST(gui::ui_font::looks_like_font(theme.text_font));
}

BOOST_AUTO_TEST_CASE(TheEditorReadsNoPresetOfTheProject_ItFallsBack)
{
	// core/res is served as res:// too: the same file under the project's protocol is still refused (#929).
	const auto& theme = select(editor_service, "res://themes/dark_neutral.desc");

	BOOST_TEST(theme.style.imgui_dark);
	BOOST_TEST(editor_service.selected().view() == "engine://themes/classic.desc");
}

BOOST_AUTO_TEST_CASE(AFontOfTheProject_IsNotRead_TheUiFontIsUsed)
{
	const std::vector<std::byte> noto = read_bytes(fs::path{ SNK_CORE_RES_DIR } / "fonts" / "NotoSans-Regular.ttf");
	const std::vector<std::byte> other = read_bytes(fs::path{ SNK_CORE_RES_DIR } / "fonts" / "lucide-icons.ttf");
	rs->store(res::tag{ "res://fonts/project.ttf" }, other);
	const res::tag preset = put("project_font.desc",
		R"({ "__type" : "gui.theme", "__version" : 1, "font" : "res://fonts/project.ttf" })");

	const auto& theme = select(preset.view());

	BOOST_TEST(!theme.style.imgui_dark);
	BOOST_TEST((theme.text_font == noto));
}

BOOST_AUTO_TEST_CASE(ATruncatedRealFont_IsReplacedByTheUiFont)
{
	std::vector<std::byte> truncated = read_bytes(fs::path{ SNK_CORE_RES_DIR } / "fonts" / "NotoSans-Regular.ttf");
	const std::vector<std::byte> noto = truncated;
	truncated.resize(truncated.size() / 2);
	rs->store(res::tag{ "memory://t/truncated.ttf" }, truncated);
	const res::tag preset = put("truncated_font.desc",
		R"({ "__type" : "gui.theme", "__version" : 1, "font" : "memory://t/truncated.ttf" })");

	const auto& theme = select(preset.view());

	BOOST_TEST(!theme.style.imgui_dark);
	BOOST_TEST((theme.text_font == noto));
}

BOOST_AUTO_TEST_CASE(APresetWithTheFontsAppliedLast_ChangesOnlyTheStyle)
{
	BOOST_TEST(select("engine://themes/dark_neutral.desc").fonts_changed);

	const auto& theme = select("engine://themes/light.desc");

	BOOST_TEST(!theme.fonts_changed);
	BOOST_TEST(theme.text_font.empty());
	BOOST_TEST(theme.style.palette.light);
}

BOOST_AUTO_TEST_CASE(APreview_AppliesAPresetWithoutAFile_AndKeepsTheSelection)
{
	select("engine://themes/dark_neutral.desc");
	gui::theme_desc edited;
	edited.accent = glm::vec3{ 1.f, 0.f, 0.f };
	edited.font_size = 16.f;
	applied.clear();

	service.preview(edited);
	settle(service);

	BOOST_REQUIRE_EQUAL(applied.size(), 1u);
	BOOST_TEST(applied.front().style.font_px == 16.f);
	BOOST_TEST(applied.front().fonts_changed);
	BOOST_TEST(applied.front().style.palette.accent.x == 1.f);
	BOOST_TEST(service.selected().view() == "engine://themes/dark_neutral.desc");
}

BOOST_AUTO_TEST_CASE(AFontOfThePersons_IsMergedWithTheUiFontForTheGlyphsItLacks)
{
	// The icon font has no Latin or Cyrillic: as a text font of the person's, every letter has to come from Noto.
	const std::vector<std::byte> icons = read_bytes(fs::path{ SNK_CORE_RES_DIR } / "fonts" / "lucide-icons.ttf");
	rs->store(res::tag{ "memory://t/own.ttf" }, icons);
	const res::tag preset = put("own_font.desc",
		R"({ "__type" : "gui.theme", "__version" : 1, "font" : "memory://t/own.ttf" })");

	const auto& theme = select(preset.view());

	BOOST_TEST((theme.text_font == icons));
	BOOST_REQUIRE(gui::ui_font::looks_like_font(theme.fallback_font));

	gui::ui_font font;
	font.set_bytes(theme.text_font);
	font.set_fallback_bytes(theme.fallback_font);
	ImFontAtlas atlas;
	ImFont* added = font.add_to(atlas, 14.f);
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());
	BOOST_TEST(added->FindGlyphNoFallback(0x0416) != nullptr); // Ж
	BOOST_TEST(added->FindGlyphNoFallback('A') != nullptr);
}

BOOST_AUTO_TEST_CASE(TheSourcesOfPresetsAndFonts_AreSetApart)
{
	put("mine/theme_in_t.desc", R"({ "__type" : "gui.theme", "__version" : 1 })");
	put("mine_other.desc", R"({ "__type" : "gui.theme", "__version" : 1 })");
	// A source is a folder, with or without its trailing '/'.
	editor_service.set_sources({ "engine://", "memory://t/mine" }, { "engine://" });

	const auto& theme = select(editor_service, "memory://t/mine/theme_in_t.desc");

	BOOST_TEST(!theme.style.imgui_dark);
	BOOST_TEST(editor_service.selected().view() == "memory://t/mine/theme_in_t.desc");
	const auto& other = select(editor_service, "memory://t/mine_other.desc");
	BOOST_TEST(other.style.imgui_dark);
}

BOOST_AUTO_TEST_CASE(APreviewWhileAChosenPresetLoads_DoesNotReplaceIt)
{
	select("engine://themes/dark_neutral.desc");
	gui::theme_desc edited;
	edited.accent = glm::vec3{ 1.f, 0.f, 0.f };
	applied.clear();

	service.select(res::tag{ "engine://themes/light.desc" });
	service.preview(edited);
	settle(service);

	BOOST_REQUIRE_EQUAL(applied.size(), 1u);
	BOOST_TEST(applied.front().style.palette.light);
	BOOST_TEST(service.selected().view() == "engine://themes/light.desc");
}

BOOST_AUTO_TEST_CASE(AColorOutsideZeroToOne_IsRefusedWithItsName)
{
	gui::theme_desc preset;
	preset.accent = glm::vec3{ 2.f, 0.f, 0.f };
	std::string problem;

	BOOST_TEST(!gui::theme_style_of(preset, problem).has_value());
	BOOST_TEST(problem.find("accent") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
