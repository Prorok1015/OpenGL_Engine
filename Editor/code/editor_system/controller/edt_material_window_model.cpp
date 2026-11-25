#include "edt_material_window_model.h"
#include "edt_data_document.h"
#include "edt_material_preview_content.h"
#include "edt_schema_inspector.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_shader_desc.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_bindings.h>
#include <snk/render/shader/rnd_shader_includes.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
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

void add_unique_tag(std::vector<res::tag>& tags, const res::tag& tag)
{
	if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
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

// The string entries of the shown `defines`, as written.
std::vector<std::string> define_entries(const json::value& shown)
{
	std::vector<std::string> out;
	const json::value* field = shown.is_object() ? shown.as_object().if_contains("defines") : nullptr;
	if (!field || !field->is_array()) return out;
	for (const json::value& entry : field->as_array()) {
		if (entry.is_string()) out.emplace_back(entry.as_string());
	}
	return out;
}

// The names `source` tests (#ifdef, #ifndef, defined) and the ones it #defines, each once, in order.
void scan_names(std::string_view source, std::vector<std::string>& tested, std::vector<std::string>& defined)
{
	// Comments blanked as the shader compiler reads them: a /* */ block may span lines.
	const std::string code = rnd::blank_comments(std::string{ source });
	for (std::string_view text = code; !text.empty();) {
		const std::size_t end = text.find('\n');
		std::string_view line = text.substr(0, end);
		text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
		line = skip_blanks(line);
		if (!line.starts_with('#')) continue;
		line = skip_blanks(line.substr(1));
		const std::string_view directive = take_name(line);
		if (directive == "define") {
			line = skip_blanks(line);
			if (const std::string_view name = take_name(line); !name.empty()) add_unique(defined, name);
			continue;
		}
		if (directive == "ifdef" || directive == "ifndef") {
			line = skip_blanks(line);
			if (const std::string_view name = take_name(line); !name.empty()) add_unique(tested, name);
			continue;
		}
		if (directive != "if" && directive != "elif") continue;
		for (std::size_t at = line.find("defined"); at != std::string_view::npos; at = line.find("defined", at + 1)) {
			if (at > 0 && is_name_char(line[at - 1])) continue;
			std::string_view rest = line.substr(at + 7);
			if (!rest.empty() && is_name_char(rest.front())) continue;
			rest = skip_blanks(rest);
			if (rest.starts_with('(')) rest = skip_blanks(rest.substr(1));
			if (const std::string_view name = take_name(rest); !name.empty()) add_unique(tested, name);
		}
	}
}

// Takes one `name` out of the sorted `set`; false when it holds none.
bool take_one(std::vector<std::string>& set, std::string_view name)
{
	const auto found = std::ranges::find(set, name);
	if (found == set.end()) return false;
	set.erase(found);
	return true;
}

bool same_place(const rnd::shader_problem& a, const rnd::shader_problem& b)
{
	return a.file == b.file && a.line == b.line && a.column == b.column && a.code == b.code && a.message == b.message;
}

// One row per place: the variants of one error are one row, their define sets in its note.
void merge_row(std::vector<edt::compile_row>& rows, edt::compile_row row)
{
	const auto same = std::ranges::find_if(rows, [&](const edt::compile_row& known) {
		return known.skinned == row.skinned && same_place(known.problem, row.problem);
	});
	if (same == rows.end()) {
		rows.push_back(std::move(row));
		return;
	}
	same->preprocess = same->preprocess || row.preprocess;
	if (!row.define_set.empty() && same->define_set.find(row.define_set) == std::string::npos) {
		same->define_set += same->define_set.empty() ? row.define_set : "; " + row.define_set;
	}
}

json::array defines_of(const json::value& shown)
{
	const json::value* field = shown.is_object() ? shown.as_object().if_contains("defines") : nullptr;
	return field && field->is_array() ? field->as_array() : json::array{};
}

// The same entries, in any order.
bool same_entries(const json::array& a, const json::array& b)
{
	const auto sorted = [](const json::array& list) -> std::optional<std::vector<std::string>> {
		std::vector<std::string> out;
		out.reserve(list.size());
		for (const json::value& entry : list) {
			if (!entry.is_string()) return std::nullopt;
			out.emplace_back(entry.as_string());
		}
		std::ranges::sort(out);
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	};
	const auto left = sorted(a);
	const auto right = sorted(b);
	return left && right ? *left == *right : a == b;
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
	std::vector<std::string> defined;
	scan_names(source, names, defined);
	// What the shader defines itself is no switch of the material.
	std::erase_if(names, [&](const std::string& name) { return holds(defined, name); });
	return names;
}

edt::material_window_model::material_window_model(data_document& document, editor_services& services)
	: m_document(document)
	, m_services(services)
{
}

edt::material_window_model::~material_window_model()
{
	unwatch_files();
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
	const json::array inherited = defines_of(m_document.base());
	const bool changed = m_document.edit([&](json::value& shown) {
		if (!shown.is_object()) return false;
		json::value& field = shown.as_object()["defines"];
		if (!field.is_array()) field = json::array{};
		const json::array before = field.as_array();
		change(field.as_array());
		// The parents' list as they have it: the edit drops the own field (apply_edit) and the material follows them again.
		if (same_entries(field.as_array(), inherited)) field = inherited;
		return !(before == field.as_array());
	});
	m_document.commit_pending();
	return changed;
}

bool edt::material_window_model::toggle(std::string_view name)
{
	if (!is_define_name(name)) return false;
	const bool on = holds(define_names(m_document.shown()), name);
	const auto named = [&](const json::value& entry) { return entry.is_string() && entry_name(entry.as_string()) == name; };
	return write_defines([&](json::array& defines) {
		if (on) {
			for (const json::value& entry : defines) {
				if (named(entry) && skip_blanks(entry.as_string()) != name) m_values.insert_or_assign(std::string{ name }, std::string{ entry.as_string() });
			}
			defines.erase(std::remove_if(defines.begin(), defines.end(), named), defines.end());
			return;
		}
		std::string entry{ name };
		if (const auto known = m_values.find(name); known != m_values.end()) {
			entry = known->second;
		} else {
			// Off here and set by a parent with a value: on again is the parent's entry.
			const json::array inherited = defines_of(m_document.base());
			if (const auto parent = std::ranges::find_if(inherited, named); parent != inherited.end()) entry = std::string_view{ parent->as_string() };
		}
		defines.emplace_back(json::string(entry));
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

bool edt::material_window_model::can_reset_to_inherited() const
{
	return !m_document.parents().empty() && m_document.own().contains("defines");
}

bool edt::material_window_model::reset_to_inherited()
{
	if (!can_reset_to_inherited()) return false;
	m_document.commit_pending();
	// The generic inspector's Reset: a request applied with the edit (apply_resets), one record.
	const bool changed = m_document.edit_marked([](json::value&, const schema_ui::field_origin& origin) {
		origin.requests.resets.emplace_back("/defines");
		return false;
	});
	m_document.commit_pending();
	return changed;
}

const edt::material_shader& edt::material_window_model::shader()
{
	scan();
	return m_shader;
}

void edt::material_window_model::scan()
{
	// A shader typed in is a sync load per value: the field is read again once its edit is committed.
	res::tag tag = m_shader.shader;
	if (!m_has_scan || !m_document.has_pending_edit()) {
		const json::value shown = m_document.shown();
		tag = res::tag{};
		if (const json::value* field = shown.is_object() ? shown.as_object().if_contains("shader") : nullptr) {
			desc::read_context context;
			context.system = m_services.descs();
			tag = desc::reference_tag(*field, context).value_or(res::tag{});
		}
	}
	if (m_scanned && tag == m_shader.shader) return;
	m_scanned = true;
	m_has_scan = true;
	++m_scan_serial;
	m_shader = material_shader{};
	m_shader.shader = tag;
	m_program.reset();
	m_shader_files.clear();
	m_source_defines.clear();
	m_shader_names.clear();
	std::vector<res::tag> files;
	const auto watch_file = [&](const res::tag& file) {
		if (std::ranges::find(files, file) == files.end()) files.push_back(file);
	};
	const auto fail = [&](std::string message) {
		if (!m_shader.error.empty()) m_shader.error += '\n';
		m_shader.error += std::move(message);
	};
	res::resource_system* resources = m_services.resources();
	if (tag.is_valid() && !resources) {
		m_shader.error = "the project resources are not available";
	} else if (tag.is_valid()) {
		watch_file(tag);
		const auto program_handle = resources->require_sync<rnd::shader_desc>(tag);
		program_handle.wait();
		if (const std::shared_ptr<rnd::shader_desc> program = program_handle.get()) {
			m_program = program;
			m_shader.vertex = program->vertex;
			m_shader.fragment = program->fragment;
			m_shader.geometry = program->geometry;
		} else {
			fail(std::format("'{}' could not be read: {}", tag.view(), program_handle.error_text()));
		}
		for (const res::tag* stage : { &m_shader.vertex, &m_shader.fragment, &m_shader.geometry }) {
			if (!stage->is_valid()) continue;
			watch_file(*stage);
			add_unique_tag(m_shader_files, *stage);
			const auto text_handle = resources->require_sync<res::text_resource>(*stage);
			text_handle.wait();
			const std::shared_ptr<res::text_resource> text = text_handle.get();
			if (!text) {
				fail(std::format("'{}' could not be read: {}", stage->view(), text_handle.error_text()));
				continue;
			}
			// The shader compiler's expansion: the same files, guards and limits.
			std::string source = text->str();
			std::vector<res::tag> included;
			std::string problems = rnd::expand_includes(source, *stage, *resources, &included);
			for (const res::tag& file : included) {
				watch_file(file);
				add_unique_tag(m_shader_files, file);
			}
			while (problems.ends_with('\n')) problems.pop_back();
			if (!problems.empty()) fail(std::move(problems));
			std::vector<std::string> tested;
			std::vector<std::string> defined;
			scan_names(source, tested, defined);
			std::erase_if(tested, [&](const std::string& name) { return holds(defined, name); });
			for (const std::string& name : tested) add_unique(m_shader_names, name);
			for (const std::string& name : defined) add_unique(m_source_defines, name);
		}
	}
	// Watched also when a read failed: the file that appears or is fixed is read again.
	sync_watches(files);
	if (!m_shader.error.empty()) egLOG_WARN("editor", "material window: {}", m_shader.error);
}

void edt::material_window_model::sync_watches(const std::vector<res::tag>& wanted)
{
	res::resource_system* resources = m_services.writable_resources();
	if (m_watching != resources) {
		unwatch_files();
		m_watching = resources;
	}
	if (!m_watching) return;
	// As data_document: a watcher registered again would miss a signal still waiting for the main thread.
	for (auto it = m_watched.begin(); it != m_watched.end();) {
		if (std::ranges::find(wanted, *it) == wanted.end()) {
			m_watching->unwatch(*it, this);
			it = m_watched.erase(it);
		} else {
			++it;
		}
	}
	for (const res::tag& tag : wanted) {
		if (std::ranges::find(m_watched, tag) != m_watched.end()) continue;
		m_watching->watch(tag, this, [this](const res::tag&) { m_scanned = false; });
		m_watched.push_back(tag);
	}
}

void edt::material_window_model::unwatch_files()
{
	if (m_watching) {
		for (const res::tag& tag : m_watched) m_watching->unwatch(tag, this);
	}
	m_watched.clear();
	m_watching = nullptr;
}

const std::vector<std::string>& edt::material_window_model::expected_defines()
{
	scan();
	if (m_has_expected && m_expected_generation == m_document.generation() && m_expected_scan == m_scan_serial) return m_expected;
	m_has_expected = true;
	m_expected_generation = m_document.generation();
	m_expected_scan = m_scan_serial;
	const std::vector<std::string> entries = define_entries(m_document.shown());
	m_expected = m_program ? rnd::program_define_entries(m_program->config(entries).cdata) : entries;
	std::ranges::sort(m_expected);
	return m_expected;
}

edt::compile_errors edt::material_window_model::compile_rows(const std::vector<rnd::shader_problem>& problems, const res::tag& preview_tag)
{
	scan();
	compile_errors out;
	out.sky = is_sky_material(m_shader);
	const std::vector<std::string>& expected = expected_defines();
	const std::string_view document = m_document.source().view();
	const auto row_of = [](const rnd::shader_problem& problem) {
		compile_row row;
		row.problem = problem;
		row.define_set = rnd::format_program_defines(rnd::parse_program_defines(problem.program));
		return row;
	};
	for (const rnd::shader_problem& problem : problems) {
		const bool own_load = problem.code == "shader/load" && !problem.program.empty()
			&& (problem.program == document || (preview_tag.is_valid() && problem.program == preview_tag.view()));
		if (own_load) {
			compile_row row = row_of(problem);
			row.define_set.clear();
			merge_row(out.material, std::move(row));
			continue;
		}
		if (std::ranges::find(m_shader_files, problem.file) == m_shader_files.end()) continue;
		compile_row row = row_of(problem);
		std::vector<std::string> set = rnd::parse_program_defines(problem.program);
		std::ranges::sort(set);
		// The pass and skinning defines the compiler adds on its own; the material's set is the rest.
		if (!holds(expected, "OPAQUE")) take_one(set, "OPAQUE");
		const bool skinned = !holds(expected, "USE_ANIMATION") && take_one(set, "USE_ANIMATION");
		if (set == expected) {
			row.skinned = skinned;
			merge_row(out.material, std::move(row));
		} else {
			if (row.define_set.empty()) row.define_set = "no defines";
			merge_row(out.others, std::move(row));
		}
	}
	for (const rnd::shader_problem& problem : m_preprocessed) {
		compile_row row = row_of(problem);
		row.preprocess = true;
		merge_row(out.material, std::move(row));
	}
	return out;
}

const edt::compile_errors& edt::material_window_model::errors(std::uint64_t generation,
	const std::function<std::vector<rnd::shader_problem>()>& snapshot, const res::tag& preview_tag)
{
	scan();
	if (m_has_errors && m_errors_generation == generation && m_errors_document == m_document.generation()
		&& m_errors_scan == m_scan_serial && m_errors_runs == m_preprocess_runs && m_errors_preview == preview_tag) {
		return m_errors;
	}
	m_has_errors = true;
	m_errors_generation = generation;
	m_errors_document = m_document.generation();
	m_errors_scan = m_scan_serial;
	m_errors_runs = m_preprocess_runs;
	m_errors_preview = preview_tag;
	m_errors = compile_rows(snapshot ? snapshot() : std::vector<rnd::shader_problem>{}, preview_tag);
	return m_errors;
}

std::vector<std::string> edt::material_window_model::pre_check()
{
	scan();
	std::vector<std::string> out;
	for (const std::string& entry : define_entries(m_document.shown())) {
		std::string_view rest = skip_blanks(entry);
		const std::string_view name = take_name(rest);
		if (name.empty() || (!rest.empty() && rest.front() != ' ' && rest.front() != '\t')) {
			out.push_back(std::format("'{}' does not start with a define name: write NAME or NAME VALUE", entry));
			continue;
		}
		std::string_view value = skip_blanks(rest);
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
		if (value.find_first_of(" \t") != std::string_view::npos) {
			out.push_back(std::format("'{}': a define takes one value token, '{}' is more than one", entry, value));
		}
		if (rnd::is_engine_define(name)) {
			out.push_back(std::format("{} is defined by the engine before the shader's source: the shader does not build", name));
		} else if (is_compiler_define(name)) {
			out.push_back(std::format("{} is set by the material compiler: setting it here changes every pass", name));
		}
		if (m_program) {
			for (const std::string& own : m_program->defines) {
				if (entry_name(own) == name && skip_blanks(own) != skip_blanks(entry)) {
					out.push_back(std::format("'{}' replaces the shader's define '{}'", entry, own));
				}
			}
		}
		if (holds(m_source_defines, name)) {
			out.push_back(std::format("{} is #defined by the shader's source as well: the macro is defined twice", name));
		}
	}
	return out;
}

bool edt::material_window_model::preprocess(std::chrono::steady_clock::time_point now, const rnd::driver::binding_table& bindings)
{
	const std::vector<std::string>& expected = expected_defines();
	std::string key{ m_shader.shader.view() };
	key += '\n' + std::to_string(m_scan_serial);
	for (const std::string& entry : expected) key += '\n' + entry;
	if (!m_preprocess_seen || key != m_preprocess_key) {
		m_preprocess_seen = true;
		m_preprocess_key = std::move(key);
		m_preprocess_pending = true;
		m_preprocess_due = now + preprocess_delay;
		return false;
	}
	if (!m_preprocess_pending || now < m_preprocess_due) return false;
	m_preprocess_pending = false;
	res::resource_system* resources = m_services.resources();
	std::vector<rnd::shader_problem> problems;
	if (m_program && resources) {
		const rnd::shader_config config = m_program->config(define_entries(m_document.shown()));
		problems = rnd::shader_config::load(config.cdata, *resources, bindings).problems;
	}
	m_preprocessed = std::move(problems);
	++m_preprocess_runs;
	return true;
}
