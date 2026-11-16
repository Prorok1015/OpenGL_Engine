#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_theme.h>

#include <cmath>

namespace {

float luminance(const ImVec4& c)
{
	return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool same_colors(const ImGuiStyle& a, const ImGuiStyle& b)
{
	for (int i = 0; i < ImGuiCol_COUNT; ++i) {
		const ImVec4& x = a.Colors[i];
		const ImVec4& y = b.Colors[i];
		if (x.x != y.x || x.y != y.y || x.z != y.z || x.w != y.w) {
			return false;
		}
	}
	return true;
}

constexpr gui::theme new_themes[] = { gui::theme::dark_neutral, gui::theme::dark_contrast, gui::theme::light };

}

BOOST_AUTO_TEST_SUITE(GuiThemeTests)

BOOST_AUTO_TEST_CASE(EveryThemeIsFoundByItsCfgName)
{
	BOOST_TEST((gui::theme_from_name("classic") == gui::theme::classic));
	BOOST_TEST((gui::theme_from_name("dark_neutral") == gui::theme::dark_neutral));
	BOOST_TEST((gui::theme_from_name("dark_contrast") == gui::theme::dark_contrast));
	BOOST_TEST((gui::theme_from_name("light") == gui::theme::light));
	BOOST_TEST(!gui::theme_from_name("Light").has_value());
	BOOST_TEST(!gui::theme_from_name("").has_value());
}

BOOST_AUTO_TEST_CASE(ClassicIsImGuisDarkStyleAtThirteenPixels)
{
	ImGuiStyle dark;
	ImGui::StyleColorsDark(&dark);
	ImGuiStyle style;
	style.WindowRounding = 9.f;

	gui::apply_theme(gui::theme::classic, style);

	BOOST_TEST(same_colors(style, dark));
	BOOST_TEST(style.WindowRounding == ImGuiStyle().WindowRounding);
	BOOST_TEST(gui::theme_font_px(gui::theme::classic) == 13.f);
}

BOOST_AUTO_TEST_CASE(EveryNewThemeKeepsTextReadableOnItsPanels)
{
	ImGuiStyle classic;
	gui::apply_theme(gui::theme::classic, classic);
	for (gui::theme t : new_themes) {
		ImGuiStyle style;
		gui::apply_theme(t, style);
		BOOST_TEST(!same_colors(style, classic));
		const float text = luminance(style.Colors[ImGuiCol_Text]);
		BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_WindowBg])) > 0.6f);
		BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_FrameBg])) > 0.5f);
		BOOST_TEST(std::fabs(text - luminance(style.Colors[ImGuiCol_Header])) > 0.4f);
	}
}

BOOST_AUTO_TEST_CASE(TheLightThemeHasDarkTextOnLightPanels)
{
	ImGuiStyle style;
	gui::apply_theme(gui::theme::light, style);
	BOOST_TEST(luminance(style.Colors[ImGuiCol_WindowBg]) > 0.8f);
	BOOST_TEST(luminance(style.Colors[ImGuiCol_Text]) < 0.2f);
}

BOOST_AUTO_TEST_SUITE_END()
