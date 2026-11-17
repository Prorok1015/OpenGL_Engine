#include <snk/gui/gui_theme_service.h>
#include <snk/gui/gui_ui_font.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>

#include <algorithm>
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

namespace
{
	// A source is a folder: "edt://themes" must not let "edt://themes_other/x.desc" through.
	std::vector<std::string> as_folders(std::vector<std::string> sources)
	{
		for (std::string& source : sources) {
			if (!source.ends_with('/')) {
				source.push_back('/');
			}
		}
		return sources;
	}
}

gui::theme_service::theme_service(res::resource_system& resources, res::tag fallback, apply_function apply,
	std::vector<std::string> preset_sources, std::vector<std::string> font_sources)
	: m_resources(resources)
	, m_fallback(std::move(fallback))
	, m_ui_font(resources.engine_file("fonts/NotoSans-Regular.ttf"))
	, m_icon_font(resources.engine_file("fonts/lucide-icons.ttf"))
	, m_apply(std::move(apply))
	, m_preset_sources(as_folders(std::move(preset_sources)))
	, m_font_sources(as_folders(std::move(font_sources)))
{
}

void gui::theme_service::set_sources(std::vector<std::string> preset_sources, std::vector<std::string> font_sources)
{
	m_preset_sources = as_folders(std::move(preset_sources));
	m_font_sources = as_folders(std::move(font_sources));
}

void gui::theme_service::select(res::tag preset)
{
	m_selected = std::move(preset);
	drop_loads();
	m_stage = stage::preset;
	if (!readable(m_selected, m_preset_sources)) {
		give_up(std::format("themes are read only from {}", sources_text(m_preset_sources)));
		return;
	}
	m_preset = m_resources.require<theme_desc>(m_selected);
}

void gui::theme_service::preview(const theme_desc& preset)
{
	if (m_stage == stage::preset) {
		// The preset chosen last is still loading: it is what the person sees next, not an edit of the one before.
		return;
	}
	drop_loads();
	std::string problem;
	if (!start(preset, problem)) {
		egLOG_WARN("gui", "theme preview not shown: {}", problem);
		m_stage = stage::idle;
	}
}

void gui::theme_service::drop_loads()
{
	m_preset = {};
	m_text_font = {};
	m_fallback_font = {};
	m_icons = {};
}

std::string gui::theme_service::sources_text(const std::vector<std::string>& sources)
{
	std::string text;
	for (const std::string& source : sources) {
		text += std::format("{}{}", text.empty() ? "" : ", ", source);
	}
	return text.empty() ? std::string{ "nowhere" } : text;
}

bool gui::theme_service::readable(const res::tag& tag, const std::vector<std::string>& sources) const
{
	if (tag == m_fallback || tag == m_ui_font || tag == m_icon_font) {
		return true;
	}
	const std::string_view text = tag.view();
	return res::portable_path_problem(tag.relative()).empty()
		&& std::ranges::any_of(sources, [&](const std::string& prefix) { return text.starts_with(prefix); });
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
	m_preset = {};
	std::string problem;
	if (!start(*preset, problem)) {
		give_up(problem);
	}
}

bool gui::theme_service::start(const theme_desc& preset, std::string& problem)
{
	const std::optional<theme_style> style = theme_style_of(preset, problem);
	if (!style) {
		return false;
	}
	m_style = *style;
	m_wants_icons = preset.icons;
	m_text_font_tag = preset.font;
	if (!readable(m_text_font_tag, m_font_sources)) {
		egLOG_WARN("gui", "theme '{}': font '{}' is not read from there ({} only), using '{}'", m_selected.view(),
			m_text_font_tag.view(), sources_text(m_font_sources), m_ui_font.view());
		m_text_font_tag = m_ui_font;
	}

	const font_key key{ m_text_font_tag, m_style.font_px, m_wants_icons };
	if (m_applied_fonts && *m_applied_fonts == key) {
		m_stage = stage::idle;
		m_apply(theme{ m_style, {}, {}, {}, false });
		return true;
	}
	m_text_font = request_font(m_text_font_tag);
	// Glyphs a font of the person's lacks (Cyrillic, say) come from the UI font, merged after it.
	m_fallback_font = m_text_font_tag == m_ui_font ? res::res_handle<res::binary_resource>{} : request_font(m_ui_font);
	m_icons = m_wants_icons ? request_font(m_icon_font) : res::res_handle<res::binary_resource>{};
	m_stage = stage::fonts;
	return true;
}

void gui::theme_service::poll_fonts()
{
	const auto settled = [](const res::res_handle<res::binary_resource>& h) {
		return !h.is_valid() || h.is_ready() || h.has_error();
	};
	if (!settled(m_text_font) || !settled(m_fallback_font) || !settled(m_icons)) {
		return;
	}

	const auto usable_bytes = [](const res::res_handle<res::binary_resource>& h) {
		const std::shared_ptr<res::binary_resource> font = h.is_ready() ? h.get() : nullptr;
		return font && ui_font::looks_like_font(font->bytes()) ? font->bytes() : std::vector<std::byte>{};
	};
	std::vector<std::byte> text = usable_bytes(m_text_font);
	std::vector<std::byte> fallback = usable_bytes(m_fallback_font);
	if (text.empty() && m_text_font_tag != m_ui_font) {
		egLOG_WARN("gui", "theme '{}': font '{}' is not a usable TTF/OTF, using '{}'", m_selected.view(),
			m_text_font_tag.view(), m_ui_font.view());
		m_text_font_tag = m_ui_font;
		text = std::move(fallback);
		fallback.clear();
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
	drop_loads();
	m_stage = stage::idle;
	m_applied_fonts = font_key{ m_text_font_tag, m_style.font_px, m_wants_icons };
	m_apply(theme{ m_style, std::move(text), std::move(fallback), std::move(icons), true });
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
