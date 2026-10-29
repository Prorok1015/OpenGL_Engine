#include "edt_build_settings_model.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <variant>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace {
	bool contains(const std::vector<std::string>& names, std::string_view name)
	{
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	json::object defines_json(const std::map<std::string, std::string>& defines)
	{
		json::object out;
		for (const auto& [name, value] : defines) out[name] = json::string(value);
		return out;
	}

	constexpr const char* k_get_result_schema = R"({
		"type": "object",
		"properties": {
			"configuration": { "type": "string" },
			"jobs_mode": { "type": "string", "enum": ["auto", "manual"] },
			"jobs": {
				"type": "object",
				"properties": {
					"manual": { "type": "integer", "minimum": 1 },
					"estimate": {
						"type": "object",
						"properties": {
							"jobs": { "type": "integer", "minimum": 1 },
							"cores": { "type": "integer", "minimum": 0 },
							"ram_bytes": { "type": "integer", "minimum": 0 }
						},
						"required": ["jobs", "cores", "ram_bytes"],
						"additionalProperties": false
					}
				},
				"required": ["estimate"],
				"additionalProperties": false
			},
			"reload_after_build": { "type": "boolean" },
			"log_verbosity": { "type": "string", "enum": ["errors", "normal", "verbose"] },
			"configurations": { "type": "object" },
			"errors": { "type": "array", "items": { "type": "string" } }
		},
		"required": ["configuration", "jobs_mode", "jobs", "reload_after_build", "log_verbosity", "configurations", "errors"],
		"additionalProperties": false
	})";
}

std::string edt::preset_for_configuration(const fs::path& code_dir, std::string_view configuration)
{
	const prj::user_presets_read_result read = prj::read_user_presets(code_dir);
	if (!read.error.empty()) {
		egLOG_WARN("editor/build", "{}: the team preset '{}' is built", read.error, configuration);
		return std::string(configuration);
	}
	return prj::has_user_preset(read.doc, configuration) ? prj::user_preset_name(configuration) : std::string(configuration);
}

fs::path edt::build_settings_model::user_presets_file() const
{
	return code_dir() / prj::k_cmake_user_presets_file_name;
}

fs::path edt::build_settings_model::build_settings_file() const
{
	return snk_dir() / k_build_settings_file;
}

edt::build_settings_model::snapshot edt::build_settings_model::read_snapshot(const fs::path& file)
{
	snapshot out;
	std::error_code ec;
	if (!fs::exists(file, ec)) return out;
	std::ifstream stream(file, std::ios::binary);
	out.exists = true;
	if (stream) out.text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	return out;
}

void edt::build_settings_model::load(const fs::path& root)
{
	m_root = root;
	m_load_errors.clear();
	m_main.reset();
	m_user_doc = nullptr;
	m_user_broken = false;
	m_loaded_configurations.clear();
	m_loaded_settings = {};
	m_main_snapshot = {};
	m_user_snapshot = {};
	m_settings_snapshot = {};

	if (root.empty()) {
		m_unavailable = "Open a project first";
		revert();
		return;
	}
	m_main_snapshot = read_snapshot(code_dir() / prj::k_cmake_presets_file_name);
	m_user_snapshot = read_snapshot(user_presets_file());
	m_settings_snapshot = read_snapshot(build_settings_file());
	if (!m_main_snapshot.exists) {
		m_unavailable = "The project has no code/CMakePresets.json";
		revert();
		return;
	}
	prj::main_presets_result main = prj::parse_cmake_presets(m_main_snapshot.text);
	if (!main.presets) {
		m_unavailable = std::format("code/CMakePresets.json cannot be read: {}", main.error);
		revert();
		return;
	}
	m_unavailable.clear();
	m_main = std::move(main.presets);

	prj::user_presets_read_result user = prj::read_user_presets(code_dir());
	if (!user.error.empty()) {
		m_user_broken = true;
		m_load_errors.push_back(std::move(user.error));
	} else {
		m_user_doc = std::move(user.doc);
	}

	if (m_settings_snapshot.exists) {
		if (auto problem = check_build_settings_file(snk_dir())) {
			m_load_errors.push_back(std::move(*problem));
		} else {
			parsed_build_settings parsed = parse_build_settings(m_settings_snapshot.text);
			for (const auto& warning : parsed.warnings) m_load_errors.push_back(std::format(".snk/build.json: {}", warning));
			m_loaded_settings = std::move(parsed.settings);
		}
	}

	for (const prj::configure_preset& preset : m_main->configure) {
		if (preset.hidden) continue;
		build_settings_configuration entry;
		entry.name = preset.name;
		entry.display_name = preset.display_name.empty() ? preset.name : preset.display_name;
		entry.generator = preset.generator;
		entry.team_defines = preset.cache_variables;
		if (!m_user_broken) entry.user_defines = prj::user_defines(m_user_doc, preset.name);
		m_loaded_configurations.push_back(std::move(entry));
	}
	revert();
}

const edt::build_settings_configuration* edt::build_settings_model::find(std::string_view configuration) const
{
	const auto it = std::find_if(m_configurations.begin(), m_configurations.end(),
		[configuration](const build_settings_configuration& entry) { return entry.name == configuration; });
	return it == m_configurations.end() ? nullptr : &*it;
}

std::string edt::build_settings_model::selected_configuration() const
{
	return m_settings.configuration.value_or(m_default_configuration);
}

std::string edt::build_settings_model::preset_for(std::string_view configuration) const
{
	return !m_user_broken && prj::has_user_preset(m_user_doc, configuration) ? prj::user_preset_name(configuration)
		: std::string(configuration);
}

std::optional<int> edt::build_settings_model::preset_jobs() const
{
	if (m_user_broken) return std::nullopt;
	return prj::user_jobs(m_user_doc, selected_configuration());
}

int edt::build_settings_model::manual_jobs() const
{
	const int jobs = m_manual_jobs.value_or(preset_jobs().value_or(m_estimate.jobs));
	return std::clamp(jobs, 1, k_max_manual_jobs);
}

void edt::build_settings_model::set_manual_jobs(int jobs)
{
	m_manual_jobs = std::clamp(jobs, 1, k_max_manual_jobs);
}

int edt::build_settings_model::presets_version() const
{
	int version = std::max(m_main ? m_main->version : 0, prj::k_min_presets_version);
	if (m_user_doc.is_object()) {
		if (const json::value* own = m_user_doc.get_object().if_contains("version"); own && own->is_int64()) {
			version = std::max(version, static_cast<int>(own->get_int64()));
		}
	}
	return version;
}

std::optional<std::string> edt::build_settings_model::set_define(std::string_view configuration, std::string_view name,
	std::string_view value)
{
	if (!m_main) return m_unavailable;
	const auto it = std::find_if(m_configurations.begin(), m_configurations.end(),
		[configuration](const build_settings_configuration& entry) { return entry.name == configuration; });
	if (it == m_configurations.end()) return std::format("CMakePresets.json has no configure preset '{}'", configuration);
	if (auto problem = prj::check_user_define(*m_main, configuration, name, value, presets_version())) return problem;
	m_define_edits.push_back(prj::set_define{ std::string(configuration), std::string(name), std::string(value) });
	it->user_defines[std::string(name)] = std::string(value);
	return std::nullopt;
}

std::optional<std::string> edt::build_settings_model::set_define_everywhere(std::string_view name, std::string_view value)
{
	if (!m_main) return m_unavailable;
	for (const auto& entry : m_configurations) {
		if (auto problem = prj::check_user_define(*m_main, entry.name, name, value, presets_version())) {
			return std::format("{}: {}", entry.display_name, *problem);
		}
	}
	for (const auto& entry : m_configurations) {
		if (auto problem = set_define(entry.name, name, value)) return problem;
	}
	return std::nullopt;
}

std::optional<std::string> edt::build_settings_model::remove_define(std::string_view configuration, std::string_view name)
{
	if (is_building()) return std::string(k_building_reason);
	const auto it = std::find_if(m_configurations.begin(), m_configurations.end(),
		[configuration](const build_settings_configuration& entry) { return entry.name == configuration; });
	if (it == m_configurations.end()) return std::format("CMakePresets.json has no configure preset '{}'", configuration);
	const auto define = it->user_defines.find(std::string(name));
	if (define == it->user_defines.end()) return std::format("user-{} has no define {}", configuration, name);
	it->user_defines.erase(define);
	m_define_edits.push_back(prj::remove_define{ std::string(configuration), std::string(name) });
	return std::nullopt;
}

bool edt::build_settings_model::settings_changed() const
{
	build_settings edited = m_settings;
	build_settings loaded = m_loaded_settings;
	edited.fresh_configure.clear();
	loaded.fresh_configure.clear();
	return edited != loaded;
}

std::vector<prj::user_presets_edit> edt::build_settings_model::pending_user_edits() const
{
	std::vector<prj::user_presets_edit> edits = m_define_edits;
	const bool mode_changed = m_settings.jobs_mode != m_loaded_settings.jobs_mode;
	if (m_settings.jobs_mode == jobs_mode::manual_jobs) {
		const bool value_changed = m_manual_jobs && preset_jobs() != *m_manual_jobs;
		if (mode_changed || value_changed) {
			for (const auto& entry : m_configurations) edits.push_back(prj::set_jobs{ entry.name, manual_jobs() });
		}
	} else if (mode_changed && !m_user_broken) {
		// Only presets that exist: clearing `jobs` must not create user-<cfg> presets nobody asked for.
		for (const auto& entry : m_configurations) {
			if (prj::has_user_preset(m_user_doc, entry.name)) edits.push_back(prj::set_jobs{ entry.name, std::nullopt });
		}
	}
	return edits;
}

std::vector<std::string> edt::build_settings_model::pending_fresh() const
{
	std::vector<std::string> out;
	for (const auto& loaded : m_loaded_configurations) {
		const build_settings_configuration* now = find(loaded.name);
		if (!now) continue;
		for (const auto& [name, value] : loaded.user_defines) {
			if (!now->user_defines.contains(name)) {
				out.push_back(loaded.name);
				break;
			}
		}
	}
	return out;
}

bool edt::build_settings_model::dirty() const
{
	return settings_changed() || !pending_user_edits().empty();
}

void edt::build_settings_model::revert()
{
	m_configurations = m_loaded_configurations;
	m_settings = m_loaded_settings;
	m_manual_jobs.reset();
	m_define_edits.clear();
}

std::vector<fs::path> edt::build_settings_model::pending_files() const
{
	std::vector<fs::path> out;
	if (!pending_user_edits().empty()) out.push_back(user_presets_file());
	if (settings_changed() || !pending_fresh().empty()) out.push_back(build_settings_file());
	return out;
}

std::vector<fs::path> edt::build_settings_model::changed_on_disk() const
{
	std::vector<fs::path> out;
	if (m_root.empty()) return out;
	const auto check = [&out](const fs::path& file, const snapshot& seen) {
		const snapshot now = read_snapshot(file);
		if (now.exists != seen.exists || now.text != seen.text) out.push_back(file);
	};
	check(code_dir() / prj::k_cmake_presets_file_name, m_main_snapshot);
	check(user_presets_file(), m_user_snapshot);
	check(build_settings_file(), m_settings_snapshot);
	return out;
}

std::optional<std::string> edt::build_settings_model::apply_blocked_reason() const
{
	if (is_building() && !pending_fresh().empty()) return std::string(k_building_reason);
	return std::nullopt;
}

bool edt::build_settings_model::refresh_fresh_configure()
{
	if (!available()) return false;
	const snapshot now = read_snapshot(build_settings_file());
	if (now.exists == m_settings_snapshot.exists && now.text == m_settings_snapshot.text) return false;
	if (!now.exists || !m_settings_snapshot.exists) return false;

	// Equal when they differ only in fresh_configure (and in keys merge_build_settings adds with their defaults).
	const auto without_fresh = [](const std::string& text) -> std::optional<std::pair<json::object, build_settings>> {
		std::error_code ec;
		json::value doc = json::parse(text, ec);
		if (ec || !doc.is_object()) return std::nullopt;
		for (const std::string_view key : { "configuration", "jobs_mode", "reload_after_build", "build_before_play",
			"log_verbosity", "fresh_configure" }) {
			doc.get_object().erase(key);
		}
		build_settings settings = parse_build_settings(text).settings;
		settings.fresh_configure.clear();
		return std::pair{ std::move(doc.get_object()), std::move(settings) };
	};
	const auto before = without_fresh(m_settings_snapshot.text);
	const auto after = without_fresh(now.text);
	if (!before || !after || before->first != after->first || before->second != after->second) return false;

	const std::vector<std::string> fresh = parse_build_settings(now.text).settings.fresh_configure;
	m_loaded_settings.fresh_configure = fresh;
	m_settings.fresh_configure = fresh;
	m_settings_snapshot = now;
	return true;
}

edt::build_settings_apply_result edt::build_settings_model::apply()
{
	build_settings_apply_result result;
	if (!available()) {
		result.user_presets_error = m_unavailable;
		return result;
	}
	const std::vector<prj::user_presets_edit> edits = pending_user_edits();
	const std::vector<std::string> fresh = pending_fresh();
	const bool write_settings = settings_changed() || !fresh.empty();
	if (auto blocked = apply_blocked_reason()) {
		result.user_presets_error = std::move(*blocked);
		return result;
	}

	// build.json first: a --fresh mark without the removal costs one configure, a removal without the mark leaves the
	// define in CMakeCache.txt.
	bool settings_ok = true;
	if (write_settings) {
		// Only the fields edited here: the rest of the file stays as it is on disk now.
		const build_settings edited = m_settings;
		const build_settings loaded = m_loaded_settings;
		auto problem = update_build_settings(snk_dir(), [&](build_settings& on_disk) {
			if (edited.configuration != loaded.configuration) on_disk.configuration = edited.configuration;
			if (edited.jobs_mode != loaded.jobs_mode) on_disk.jobs_mode = edited.jobs_mode;
			if (edited.reload_after_build != loaded.reload_after_build) on_disk.reload_after_build = edited.reload_after_build;
			if (edited.log_verbosity != loaded.log_verbosity) on_disk.log_verbosity = edited.log_verbosity;
			for (const auto& configuration : fresh) {
				if (!contains(on_disk.fresh_configure, configuration)) on_disk.fresh_configure.push_back(configuration);
			}
		}, m_write);
		if (problem) {
			settings_ok = false;
			result.build_settings_error = std::move(*problem);
		} else {
			m_settings_snapshot = read_snapshot(build_settings_file());
			result.fresh = fresh;
		}
	}

	if (!edits.empty()) {
		if (!settings_ok && !fresh.empty()) {
			result.user_presets_error = "not written: removing a define needs a --fresh configure, which .snk/build.json "
				"could not record";
		} else {
			const prj::user_presets_save_result saved = prj::save_user_presets(code_dir(), edits, m_write);
			if (!saved.error.empty()) {
				result.user_presets_error = saved.error;
			} else {
				m_user_snapshot = read_snapshot(user_presets_file());
			}
		}
	}

	if (result.ok()) load(m_root);
	return result;
}

std::vector<edt::registration> edt::register_build_settings_commands(cmd::command_registry& commands,
	build_settings_command_options options)
{
	std::vector<registration> out;

	cmd::command_desc get;
	get.id = "build.settings.get";
	get.summary = "The game build settings of the open project: configuration, jobs, module reload, log verbosity, and the "
	get.category = cmd::agent_category::read;
		"CMake defines per configuration (team ones from CMakePresets.json, the user's from CMakeUserPresets.json)";
	get.args_schema = cmd::no_args_schema();
	get.result_schema = json::parse(k_get_result_schema);
	get.mutates = false;
	get.risk = cmd::danger::none;
	get.invoke = [options = std::move(options)](const cmd::call&) {
		const std::optional<fs::path> root = options.project_root ? options.project_root() : std::nullopt;
		if (!root) return cmd::result::failure("no_project", "No project is open");

		build_settings_model model;
		model.set_default_configuration(options.default_configuration);
		if (options.estimate) model.set_estimate(options.estimate());
		model.load(*root);
		if (!model.available()) return cmd::result::failure(cmd::codes::failed, model.unavailable_reason());

		const build_settings& settings = model.settings();
		const jobs_estimate& estimate = model.estimate();
		json::object jobs{ { "estimate", json::object{
			{ "jobs", std::max(1, estimate.jobs) },
			{ "cores", estimate.cores },
			{ "ram_bytes", estimate.ram } } } };
		// A hand-written 0 or negative `jobs` is not a manual value CMake would use.
		if (const auto manual = model.preset_jobs(); manual && *manual >= 1) jobs["manual"] = *manual;

		json::object configurations;
		for (const auto& entry : model.configurations()) {
			configurations[entry.name] = json::object{
				{ "preset", json::string(model.preset_for(entry.name)) },
				{ "team_defines", defines_json(entry.team_defines) },
				{ "user_defines", defines_json(entry.user_defines) } };
		}
		json::array errors;
		for (const auto& error : model.load_errors()) errors.emplace_back(json::string(error));

		return cmd::result::success(json::object{
			{ "configuration", json::string(model.selected_configuration()) },
			{ "jobs_mode", json::string(jobs_mode_name(settings.jobs_mode)) },
			{ "jobs", std::move(jobs) },
			{ "reload_after_build", settings.reload_after_build },
			{ "log_verbosity", json::string(log_verbosity_name(settings.log_verbosity)) },
			{ "configurations", std::move(configurations) },
			{ "errors", std::move(errors) } });
	};
	cmd::registration added = commands.add(std::move(get));
	if (added.active()) out.push_back(std::move(added));
	return out;
}
