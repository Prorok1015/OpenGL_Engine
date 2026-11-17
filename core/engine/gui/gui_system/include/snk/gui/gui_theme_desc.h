#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/model_codec/res_tag.h>
#include <snk/common/snk_api.h>

#include <boost/describe.hpp>
#include <glm/glm.hpp>

#include <string_view>

namespace gui
{
	// imgui_dark: ImGui's own dark style (the classic look); the palette and the sizes are not used.
	enum class theme_base { imgui_dark, palette };
	BOOST_DESCRIBE_ENUM(theme_base, imgui_dark, palette)

	class theme_desc;
}

namespace desc {
	template<>
	inline constexpr auto field_attrs<gui::theme_desc> = attrs(
		f("base").tooltip("imgui_dark: ImGui's own dark style; palette: the colors and sizes below"),
		f("light").tooltip("Dark text on light panels: the shades that tint rows and dim modals turn dark"),
		f("window").color().tooltip("Title bars, menu bar, tab bar, empty dock space"),
		f("panel").color().tooltip("Window contents and the selected tab"),
		f("frame").color().tooltip("Input fields and buttons"),
		f("frame_hover").color(),
		f("border").color(),
		f("text").color(),
		f("text_dim").color().tooltip("Disabled text and plot lines"),
		f("accent").color().tooltip("Check marks, sliders, the selected tab's line"),
		f("accent_hover").color(),
		f("selection").color().tooltip("Selected rows and pressed buttons"),
		f("rounding").range(0, 16, 1),
		f("scrollbar_rounding").range(0, 16, 1),
		f("window_padding").range(0, 32, 1),
		f("frame_padding").range(0, 32, 1),
		f("item_spacing").range(0, 32, 1),
		f("scrollbar_size").range(4, 32, 1),
		f("border_size").range(0, 4, 1),
		f("tab_overline").range(0, 8, 1),
		f("font").tooltip("TTF/OTF of the UI from engine://, res:// or this file's own protocol"),
		f("font_size").range(8, 48, 1).tooltip("Pixels before the monitor's scale"),
		f("icons").tooltip("Merge the editor's icon font into the UI font"));
}

namespace gui
{
	// A theme preset of the editor's UI (#1109): data, not code. The shipped ones are engine://themes/*.desc;
	// gui::theme_service loads one and resolves it into a gui::theme_style.
	class SNK_API theme_desc : public desc::schema_desc<theme_desc>
	{
	public:
		static constexpr std::string_view __type = "gui.theme";
		using base_type = desc::desc_base;

		theme_base base = theme_base::palette;
		bool light = false;
		glm::vec3 window{ 0.1171875f, 0.12109375f, 0.1328125f };
		glm::vec3 panel{ 0.1484375f, 0.15625f, 0.16796875f };
		glm::vec3 frame{ 0.203125f, 0.2109375f, 0.23046875f };
		glm::vec3 frame_hover{ 0.24609375f, 0.2578125f, 0.28125f };
		glm::vec3 border{ 0.22265625f, 0.23046875f, 0.25f };
		glm::vec3 text{ 0.875f, 0.87890625f, 0.8984375f };
		glm::vec3 text_dim{ 0.5234375f, 0.5390625f, 0.56640625f };
		glm::vec3 accent{ 0.23828125f, 0.48046875f, 0.87890625f };
		glm::vec3 accent_hover{ 0.33203125f, 0.5625f, 0.9296875f };
		glm::vec3 selection{ 0.1796875f, 0.26171875f, 0.41796875f };
		float rounding = 4.f;
		float scrollbar_rounding = 6.f;
		glm::vec2 window_padding{ 8.f, 8.f };
		glm::vec2 frame_padding{ 8.f, 4.f };
		glm::vec2 item_spacing{ 8.f, 6.f };
		float scrollbar_size = 12.f;
		float border_size = 1.f;
		float tab_overline = 2.f;
		res::tag font{ "engine://fonts/NotoSans-Regular.ttf" };
		float font_size = 14.f;
		bool icons = true;

		BOOST_DESCRIBE_CLASS(theme_desc, (), (base, light, window, panel, frame, frame_hover, border, text, text_dim,
			accent, accent_hover, selection, rounding, scrollbar_rounding, window_padding, frame_padding, item_spacing,
			scrollbar_size, border_size, tab_overline, font, font_size, icons), (), ())
	};
}
