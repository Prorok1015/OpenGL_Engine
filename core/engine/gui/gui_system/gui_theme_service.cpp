#include <snk/gui/gui_theme_service.h>
#include <snk/gui/gui_ui_font.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>

#include <cmath>
#include <format>
#include <utility>

namespace
{
	constexpr float k_min_font_px = 8.f;
	constexpr float k_max_font_px = 48.f;
	constexpr float k_max_size = 64.f;

	bool unit_color(const glm::vec3& c)
	{
		for (int i = 0; i < 3; ++i) {
			if (!std::isfinite(c[i]) || c[i] < 0.f || c[i] > 1.f) {
				return false;
			}
		}
		return true;
	}

	bool size_in_range(float v)
	{
		return std::isfinite(v) && v >= 0.f && v <= k_max_size;
	}

	bool size_in_range(const glm::vec2& v)
	{
		return size_in_range(v.x) && size_in_range(v.y);
	}

	ImVec4 color_of(const glm::vec3& c)
	{
		return ImVec4(c.r, c.g, c.b, 1.f);
	}

	ImVec2 size_of(const glm::vec2& v)
	{
		return ImVec2(v.x, v.y);
	}
}

std::optional<gui::theme_style> gui::theme_style_of(const theme_desc& preset, std::string& problem)
{
	if (!std::isfinite(preset.font_size) || preset.font_size < k_min_font_px || preset.font_size > k_max_font_px) {
		problem = std::format("font_size {} is outside {}..{}", preset.font_size, k_min_font_px, k_max_font_px);
		return std::nullopt;
	}
	if (!preset.font.is_valid()) {
		problem = "font is empty";
		return std::nullopt;
	}
	theme_style style;
	style.font_px = preset.font_size;
	style.imgui_dark = preset.base == theme_base::imgui_dark;
	if (style.imgui_dark) {
		return style;
	}

	const std::pair<const char*, const glm::vec3*> colors[] = {
		{ "window", &preset.window }, { "panel", &preset.panel }, { "frame", &preset.frame },
		{ "frame_hover", &preset.frame_hover }, { "border", &preset.border }, { "text", &preset.text },
		{ "text_dim", &preset.text_dim }, { "accent", &preset.accent }, { "accent_hover", &preset.accent_hover },
		{ "selection", &preset.selection },
	};
	for (const auto& [name, color] : colors) {
		if (!unit_color(*color)) {
			problem = std::format("color {} is not three numbers in 0..1", name);
			return std::nullopt;
		}
	}
	const bool sizes_ok = size_in_range(preset.rounding) && size_in_range(preset.scrollbar_rounding)
		&& size_in_range(preset.window_padding) && size_in_range(preset.frame_padding)
		&& size_in_range(preset.item_spacing) && size_in_range(preset.scrollbar_size)
		&& size_in_range(preset.border_size) && size_in_range(preset.tab_overline);
	if (!sizes_ok) {
		problem = std::format("a size is outside 0..{}", k_max_size);
		return std::nullopt;
	}

	theme_palette& p = style.palette;
	p.window = color_of(preset.window);
	p.panel = color_of(preset.panel);
	p.frame = color_of(preset.frame);
	p.frame_hover = color_of(preset.frame_hover);
	p.border = color_of(preset.border);
	p.text = color_of(preset.text);
	p.text_dim = color_of(preset.text_dim);
	p.accent = color_of(preset.accent);
	p.accent_hover = color_of(preset.accent_hover);
	p.selection = color_of(preset.selection);
	p.light = preset.light;

	theme_metrics& m = style.metrics;
	m.rounding = preset.rounding;
	m.scrollbar_rounding = preset.scrollbar_rounding;
	m.window_padding = size_of(preset.window_padding);
	m.frame_padding = size_of(preset.frame_padding);
	m.item_spacing = size_of(preset.item_spacing);
	m.scrollbar_size = preset.scrollbar_size;
	m.border_size = preset.border_size;
	m.tab_overline = preset.tab_overline;
	return style;
}

gui::theme_service::theme_service(res::resource_system& resources, res::tag fallback, apply_function apply)
	: m_resources(resources)
	, m_fallback(std::move(fallback))
	, m_ui_font(resources.engine_file("fonts/NotoSans-Regular.ttf"))
	, m_icon_font(resources.engine_file("fonts/lucide-icons.ttf"))
	, m_apply(std::move(apply))
{
}

void gui::theme_service::select(res::tag preset)
{
	m_selected = std::move(preset);
	m_text_font = {};
	m_icons = {};
	m_preset = m_resources.require<theme_desc>(m_selected);
	m_stage = stage::preset;
}

void gui::theme_service::poll()
{
	if (m_stage == stage::preset) {
		poll_preset();
	}
	if (m_stage == stage::fonts) {
		poll_fonts();
	}
}

void gui::theme_service::poll_preset()
{
	if (m_preset.has_error()) {
		give_up(m_preset.error_text());
		return;
	}
	const std::shared_ptr<theme_desc> preset = m_preset.get();
	if (!preset) {
		return;
	}
	std::string problem;
	const std::optional<theme_style> style = theme_style_of(*preset, problem);
	if (!style) {
		give_up(problem);
		return;
	}
	m_style = *style;
	m_wants_icons = preset->icons;
	m_text_font_tag = preset->font;
	m_text_font = request_font(m_text_font_tag);
	m_icons = m_wants_icons ? request_font(m_icon_font) : res::res_handle<res::binary_resource>{};
	m_preset = {};
	m_stage = stage::fonts;
}

void gui::theme_service::poll_fonts()
{
	const auto settled = [](const res::res_handle<res::binary_resource>& h) {
		return !h.is_valid() || h.is_ready() || h.has_error();
	};
	if (!settled(m_text_font) || !settled(m_icons)) {
		return;
	}

	const auto usable_bytes = [](const res::res_handle<res::binary_resource>& h) {
		const std::shared_ptr<res::binary_resource> font = h.is_ready() ? h.get() : nullptr;
		return font && ui_font::looks_like_font(font->bytes()) ? font->bytes() : std::vector<std::byte>{};
	};
	std::vector<std::byte> text = usable_bytes(m_text_font);
	if (text.empty() && m_text_font_tag != m_ui_font) {
		egLOG_WARN("gui", "theme '{}': font '{}' is not a usable TTF/OTF, using '{}'", m_selected.view(),
			m_text_font_tag.view(), m_ui_font.view());
		m_text_font_tag = m_ui_font;
		m_text_font = request_font(m_ui_font);
		return;
	}
	if (text.empty()) {
		egLOG_WARN("gui", "theme '{}': the UI font '{}' cannot be used, using the built-in ImGui font",
			m_selected.view(), m_ui_font.view());
	}
	std::vector<std::byte> icons = usable_bytes(m_icons);
	if (m_wants_icons && icons.empty()) {
		egLOG_WARN("gui", "theme '{}': icon font '{}' cannot be used, the UI shows text placeholders",
			m_selected.view(), m_icon_font.view());
	}
	m_text_font = {};
	m_icons = {};
	m_stage = stage::idle;
	m_apply(theme{ m_style, std::move(text), std::move(icons) });
}

void gui::theme_service::give_up(std::string_view why)
{
	m_preset = {};
	if (m_selected == m_fallback) {
		egLOG_WARN("gui", "theme '{}' cannot be used ({}), keeping the current look", m_selected.view(), why);
		m_stage = stage::idle;
		return;
	}
	egLOG_WARN("gui", "theme '{}' cannot be used ({}), using '{}'", m_selected.view(), why, m_fallback.view());
	select(m_fallback);
}

res::res_handle<res::binary_resource> gui::theme_service::request_font(const res::tag& tag)
{
	if (!m_resources.exists(tag)) {
		return m_resources.refused<res::binary_resource>(tag, "no such file");
	}
	return m_resources.require<res::binary_resource>(tag);
}
