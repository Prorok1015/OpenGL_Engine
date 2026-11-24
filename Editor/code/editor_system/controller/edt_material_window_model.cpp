#include "edt_material_window_model.h"
#include "edt_data_document.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_shader_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <memory>
#include <utility>

namespace json = boost::json;

namespace {

bool is_name_start(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_name_char(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string_view skip_blanks(std::string_view text)
{
	const std::size_t first = text.find_first_not_of(" \t");
	return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

// The identifier at the start of `text` (empty: none) and what follows it.
std::string_view take_name(std::string_view& text)
{
	if (text.empty() || !is_name_start(text.front())) return {};
	std::size_t length = 1;
	while (length < text.size() && is_name_char(text[length])) ++length;
	const std::string_view name = text.substr(0, length);
	text.remove_prefix(length);
	return name;
}

void add_unique(std::vector<std::string>& names, std::string_view name)
{
	if (std::ranges::find(names, name) == names.end()) names.emplace_back(name);
}

// "NAME" or "NAME VALUE": the define an entry of `defines` sets.
std::string_view entry_name(std::string_view entry)
{
	entry = skip_blanks(entry);
	return entry.substr(0, entry.find_first_of(" \t"));
}

std::vector<std::string> define_names(const json::value& shown)
{
	std::vector<std::string> names;
	const json::value* field = shown.is_object() ? shown.as_object().if_contains("defines") : nullptr;
	if (!field || !field->is_array()) return names;
	for (const json::value& entry : field->as_array()) {
		if (!entry.is_string()) continue;
		const std::string_view name = entry_name(entry.as_string());
		if (!name.empty()) add_unique(names, name);
	}
	return names;
}

bool holds(const std::vector<std::string>& names, std::string_view name)
{
	return std::ranges::find(names, name) != names.end();
}

}

bool edt::is_compiler_define(std::string_view name)
{
	return name == "USE_ANIMATION" || name == "OPAQUE";
}

bool edt::is_define_name(std::string_view name)
{
	return !name.empty() && is_name_start(name.front()) && std::ranges::all_of(name, is_name_char);
}

std::vector<std::string> edt::scan_define_names(std::string_view source)
{
	std::vector<std::string> names;
	while (!source.empty()) {
		const std::size_t end = source.find('\n');
		std::string_view line = source.substr(0, end);
		source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
		line = line.substr(0, line.find("//"));
		line = skip_blanks(line);
		if (!line.starts_with('#')) continue;
		line = skip_blanks(line.substr(1));
		const std::string_view directive = take_name(line);
		if (directive == "ifdef" || directive == "ifndef") {
			line = skip_blanks(line);
			if (const std::string_view name = take_name(line); !name.empty()) add_unique(names, name);
			continue;
		}
		if (directive != "if" && directive != "elif") continue;
		for (std::size_t at = line.find("defined"); at != std::string_view::npos; at = line.find("defined", at + 1)) {
			if (at > 0 && is_name_char(line[at - 1])) continue;
			std::string_view rest = line.substr(at + 7);
			if (!rest.empty() && is_name_char(rest.front())) continue;
			rest = skip_blanks(rest);
			if (rest.starts_with('(')) rest = skip_blanks(rest.substr(1));
			if (const std::string_view name = take_name(rest); !name.empty()) add_unique(names, name);
		}
	}
	return names;
}

edt::material_window_model::material_window_model(data_document& document, editor_services& services)
	: m_document(document)
	, m_services(services)
{
}

std::vector<edt::define_row> edt::material_window_model::rows()
{
	scan();
	const json::value shown = m_document.shown();
	const std::vector<std::string> on = define_names(shown);
	const std::vector<std::string> inherited = define_names(m_document.base());
	const bool own_list = m_document.own().contains("defines");

	std::vector<std::string> names = on;
	for (const std::vector<std::string>* source : std::array<const std::vector<std::string>*, 3>{ &inherited, &m_added, &m_shader_names }) {
		for (const std::string& name : *source) add_unique(names, name);
	}
	std::ranges::sort(names);

	std::vector<define_row> out;
	out.reserve(names.size());
	for (std::string& name : names) {
		if (is_compiler_define(name)) continue;
		define_row row;
		row.state = !holds(on, name) ? define_state::off : own_list ? define_state::own_on : define_state::inherited_on;
		row.inherited = holds(inherited, name);
		row.from_shader = holds(m_shader_names, name);
		row.name = std::move(name);
		out.push_back(std::move(row));
	}
	return out;
}

bool edt::material_window_model::write_defines(const std::function<void(json::array& defines)>& change)
{
	// A toggle is a click, not a drag: it is a record of its own, apart from an edit of a field still pending.
	m_document.commit_pending();
	const bool changed = m_document.edit([&](json::value& shown) {
		if (!shown.is_object()) return false;
		json::value& field = shown.as_object()["defines"];
		if (!field.is_array()) field = json::array{};
		const json::array before = field.as_array();
		change(field.as_array());
		return !(before == field.as_array());
	});
	m_document.commit_pending();
	return changed;
}

bool edt::material_window_model::toggle(std::string_view name)
{
	if (!is_define_name(name)) return false;
	const bool on = holds(define_names(m_document.shown()), name);
	return write_defines([&](json::array& defines) {
		if (on) {
			const auto named = [&](const json::value& entry) { return entry.is_string() && entry_name(entry.as_string()) == name; };
			defines.erase(std::remove_if(defines.begin(), defines.end(), named), defines.end());
		} else {
			defines.emplace_back(json::string(name));
		}
	});
}

std::string edt::material_window_model::add(std::string_view name)
{
	if (!is_define_name(name)) {
		return std::format("'{}' is not a define name: a letter or '_', then letters, digits or '_'", name);
	}
	if (is_compiler_define(name)) {
		return std::format("{} is set by the material compiler", name);
	}
	if (holds(define_names(m_document.shown()), name)) {
		return std::format("{} is on already", name);
	}
	if (!toggle(name)) {
		return std::format("{} could not be added", name);
	}
	add_unique(m_added, name);
	return {};
}

bool edt::material_window_model::remove(std::string_view name)
{
	std::erase(m_added, name);
	if (!holds(define_names(m_document.shown()), name)) return false;
	return toggle(name);
}

const edt::material_shader& edt::material_window_model::shader()
{
	scan();
	return m_shader;
}

void edt::material_window_model::scan()
{
	const json::value shown = m_document.shown();
	res::tag tag;
	if (const json::value* field = shown.is_object() ? shown.as_object().if_contains("shader") : nullptr) {
		desc::read_context context;
		context.system = m_services.descs();
		tag = desc::reference_tag(*field, context).value_or(res::tag{});
	}
	if (m_scanned && tag == m_shader.shader) return;
	m_scanned = true;
	m_shader = material_shader{};
	m_shader.shader = tag;
	m_shader_names.clear();
	if (!tag.is_valid()) return;

	res::resource_system* resources = m_services.resources();
	if (!resources) {
		m_shader.error = "the project resources are not available";
		return;
	}
	const auto program_handle = resources->require_sync<rnd::shader_desc>(tag);
	program_handle.wait();
	const std::shared_ptr<rnd::shader_desc> program = program_handle.get();
	if (!program) {
		m_shader.error = std::format("'{}' could not be read: {}", tag.view(), program_handle.error_text());
		egLOG_WARN("editor", "material window: {}", m_shader.error);
		return;
	}
	m_shader.vertex = program->vertex;
	m_shader.fragment = program->fragment;
	m_shader.geometry = program->geometry;
	for (const res::tag* stage : { &m_shader.vertex, &m_shader.fragment, &m_shader.geometry }) {
		if (!stage->is_valid()) continue;
		const auto text_handle = resources->require_sync<res::text_resource>(*stage);
		text_handle.wait();
		const std::shared_ptr<res::text_resource> text = text_handle.get();
		if (!text) {
			if (!m_shader.error.empty()) m_shader.error += '\n';
			m_shader.error += std::format("'{}' could not be read: {}", stage->view(), text_handle.error_text());
			continue;
		}
		for (const std::string& name : scan_define_names(text->str())) add_unique(m_shader_names, name);
	}
	if (!m_shader.error.empty()) egLOG_WARN("editor", "material window: {}", m_shader.error);
}
