#include "edt_theme_settings.h"
#include "../core/edt_settings.h"

#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/desc/desc_system.h>
#include <snk/gui/gui_theme_service.h>
#include <snk/resource/res_system.h>

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <utility>

namespace json = boost::json;

namespace
{
	constexpr std::string_view k_theme_key = "theme";

	json::object object_of(std::string_view text)
	{
		boost::system::error_code ec;
		json::value value = json::parse(text, ec);
		if (ec || !value.is_object()) {
			return {};
		}
		return std::move(value.as_object());
	}

	bool ends_with_lower(std::string_view text, std::string_view suffix)
	{
		if (text.size() < suffix.size()) {
			return false;
		}
		const std::string_view tail = text.substr(text.size() - suffix.size());
		return std::equal(tail.begin(), tail.end(), suffix.begin(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == b;
		});
	}

	// CON, NUL, COM1...: a file of that name cannot be made on Windows.
	bool windows_device_name(std::string_view name)
	{
		std::string upper(name);
		std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
			return true;
		}
		return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT"))
			&& upper[3] >= '0' && upper[3] <= '9';
	}

	using list_function = std::function<std::vector<res::tag>(std::string_view, std::string_view)>;

	std::vector<res::tag> sorted_files(const res::resource_system& resources, const list_function& list,
		std::string_view protocol, std::string_view folder, std::initializer_list<std::string_view> extensions)
	{
		std::vector<res::tag> out;
		const std::vector<res::tag> files = list ? list(protocol, folder)
			: resources.files_under(protocol, folder).value_or(std::vector<res::tag>{});
		for (const res::tag& tag : files) {
			const bool wanted = std::ranges::any_of(extensions, [&](std::string_view e) {
				return ends_with_lower(tag.view(), e);
			});
			if (wanted) {
				out.push_back(tag);
			}
		}
		std::ranges::sort(out, {}, [](const res::tag& t) { return t.view(); });
		return out;
	}
}

std::optional<res::tag> edt::chosen_theme_of(std::string_view json_text)
{
	const json::object object = object_of(json_text);
	const json::value* theme = object.if_contains(k_theme_key);
	if (!theme || !theme->is_string() || theme->as_string().empty()) {
		return std::nullopt;
	}
	return res::tag{ std::string_view{ theme->as_string() } };
}

std::string edt::with_chosen_theme(std::string_view json_text, const res::tag& theme)
{
	json::object object = object_of(json_text);
	object[k_theme_key] = theme.view();
	return json::serialize(object);
}

bool edt::valid_theme_name(std::string_view name)
{
	if (name.empty() || name.size() > 64) {
		return false;
	}
	return !windows_device_name(name) && std::ranges::all_of(name, [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
	});
}

res::tag edt::personal_theme_tag(std::string_view name)
{
	return res::tag{ std::format("{}{}.desc", k_personal_themes, name) };
}

bool edt::is_personal_theme(const res::tag& tag)
{
	return tag.view().starts_with(k_personal_themes);
}

std::string edt::theme_name_of(const res::tag& tag)
{
	std::string_view name = tag.view();
	if (const size_t slash = name.find_last_of('/'); slash != std::string_view::npos) {
		name.remove_prefix(slash + 1);
	}
	if (ends_with_lower(name, ".desc")) {
		name.remove_suffix(5);
	}
	return std::string{ name };
}

edt::theme_settings_model::theme_settings_model(ports from)
	: m_ports(std::move(from))
{
}

void edt::theme_settings_model::start()
{
	if (!available()) {
		return;
	}
	m_ports.themes->set_sources({ "engine://", std::string{ k_personal_themes } },
		{ "engine://", std::string{ k_personal_fonts } });
	refresh_lists();
	load_working(current());
	if (m_ports.theme_set_by_config) {
		return;
	}
	const res::tag choice = settings_tag(k_theme_setting);
	if (m_ports.resources.exists(choice)) {
		m_choice = m_ports.resources.require<res::text_resource>(choice);
	}
}

void edt::theme_settings_model::poll()
{
	if (m_choice.is_valid() && (m_choice.is_ready() || m_choice.has_error())) {
		const std::shared_ptr<res::text_resource> text = m_choice.get();
		m_choice = {};
		const std::optional<res::tag> chosen = text ? chosen_theme_of(text->str()) : std::nullopt;
		if (chosen && (chosen->view().starts_with("engine://") || is_personal_theme(*chosen))) {
			m_ports.themes->select(*chosen);
			load_working(*chosen);
		} else if (chosen) {
			egLOG_WARN("editor/settings", "edt://settings/{} names {}, which is no preset of engine:// or {}: "
				"keeping the default", k_theme_setting, chosen->view(), k_personal_themes);
		} else {
			egLOG_WARN("editor/settings", "edt://settings/{} names no theme, keeping the default", k_theme_setting);
		}
	}
	if (m_loading.is_valid()) {
		if (const std::shared_ptr<gui::theme_desc> loaded = m_loading.get()) {
			m_working = std::make_unique<gui::theme_desc>(*loaded);
			m_dirty = false;
			m_loading = {};
		} else if (m_loading.has_error()) {
			m_message = std::format("{} could not be read: {}", m_working_tag.view(), m_loading.error_text());
			m_loading = {};
		}
	}
}

void edt::theme_settings_model::refresh_lists()
{
	const auto& list = m_ports.list_files;
	m_presets = sorted_files(m_ports.resources, list, "engine", "themes", { ".desc" });
	if (m_presets.empty()) {
		// Without an engine:// mount the shipped presets are found through engine_file.
		for (const char* name : { "classic", "dark_neutral", "dark_contrast", "light" }) {
			m_presets.push_back(m_ports.resources.engine_file(std::format("themes/{}.desc", name)));
		}
	}
	for (res::tag& tag : sorted_files(m_ports.resources, list, "edt", "themes", { ".desc" })) {
		m_presets.push_back(std::move(tag));
	}
	m_fonts = { m_ports.resources.engine_file("fonts/NotoSans-Regular.ttf") };
	for (res::tag& tag : sorted_files(m_ports.resources, list, "edt", "fonts", { ".ttf", ".otf" })) {
		m_fonts.push_back(std::move(tag));
	}
}

res::tag edt::theme_settings_model::current() const
{
	return available() ? m_ports.themes->selected() : res::tag{};
}

void edt::theme_settings_model::choose(const res::tag& preset)
{
	if (!available()) {
		return;
	}
	// The kept choice still being read must not undo this one.
	m_choice = {};
	m_ports.themes->select(preset);
	remember_choice(preset);
	load_working(preset);
}

void edt::theme_settings_model::load_working(const res::tag& preset)
{
	m_working.reset();
	m_dirty = false;
	m_working_tag = preset;
	m_loading = preset.is_valid() ? m_ports.resources.require<gui::theme_desc>(preset)
		: res::res_handle<gui::theme_desc>{};
}

void edt::theme_settings_model::edited()
{
	if (!available() || !m_working) {
		return;
	}
	m_dirty = true;
	m_ports.themes->preview(*m_working);
}

std::optional<std::string> edt::theme_settings_model::save()
{
	if (!m_working || !editable()) {
		return m_message = "only a preset of edt://themes/ can be saved: duplicate a built-in one first";
	}
	if (std::optional<std::string> problem = write_preset(m_working_tag, *m_working)) {
		return m_message = *problem;
	}
	m_dirty = false;
	m_message = std::format("saved {}", m_working_tag.view());
	m_choice = {};
	m_ports.themes->select(m_working_tag);
	remember_choice(m_working_tag);
	return std::nullopt;
}

void edt::theme_settings_model::revert()
{
	if (!available()) {
		return;
	}
	m_ports.resources.evict_cached(m_working_tag);
	m_ports.themes->select(m_working_tag);
	load_working(m_working_tag);
	m_message.clear();
}

std::optional<std::string> edt::theme_settings_model::duplicate(std::string_view name)
{
	if (!m_working) {
		return m_message = "the preset is still loading";
	}
	if (!valid_theme_name(name)) {
		return m_message = "a name is 1 to 64 letters, digits, '_' or '-'";
	}
	const res::tag tag = personal_theme_tag(name);
	if (m_ports.resources.exists(tag)) {
		return m_message = std::format("{} exists already", tag.view());
	}
	gui::theme_desc copy = *m_working;
	// A shipped preset's font stays in engine://; one of the person's own fonts is in edt://fonts/ already.
	if (std::optional<std::string> problem = write_preset(tag, copy)) {
		return m_message = *problem;
	}
	refresh_lists();
	m_message = std::format("made {}", tag.view());
	choose(tag);
	return std::nullopt;
}

std::optional<std::string> edt::theme_settings_model::write_preset(const res::tag& tag, const gui::theme_desc& preset)
{
	json::object body;
	preset.serialize(body);
	body["__type"] = gui::theme_desc::__type;
	m_ports.descs.stamp_tree(body);
	const std::vector<std::byte> bytes = desc::pretty_json_bytes(json::value{ std::move(body) });
	try {
		if (!m_ports.resources.store_sync(tag, bytes)) {
			return std::format("{} cannot be written", tag.view());
		}
	} catch (const std::logic_error& e) {
		return std::format("{} cannot be written: {}", tag.view(), e.what());
	}
	m_ports.resources.signal_changed(tag);
	return std::nullopt;
}

void edt::theme_settings_model::remember_choice(const res::tag& preset)
{
	// The file holds the choice alone: it is written whole, without a blocking read first.
	if (!write_setting(m_ports.resources, k_theme_setting, with_chosen_theme("", preset))) {
		m_message = std::format("the choice could not be kept in edt://settings/{}", k_theme_setting);
	}
}
