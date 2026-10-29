#include "edt_build_settings.h"
#include "edt_path_text.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace json = boost::json;
namespace fs = std::filesystem;
using edt::utf8_of;

namespace {
	constexpr std::string_view k_configuration = "configuration";
	constexpr std::string_view k_jobs_mode = "jobs_mode";
	constexpr std::string_view k_reload_after_build = "reload_after_build";
	constexpr std::string_view k_build_before_play = "build_before_play";
	constexpr std::string_view k_log_verbosity = "log_verbosity";
	constexpr std::string_view k_fresh_configure = "fresh_configure";

	std::string bad_value(std::string_view key, const json::value& value, std::string_view expected)
	{
		return std::format("'{}' is {}, expected {}: the default is used", key, json::serialize(value), expected);
	}

	void read_bool(const json::object& doc, std::string_view key, bool& out, std::vector<std::string>& warnings)
	{
		const json::value* value = doc.if_contains(key);
		if (!value || value->is_null()) return;
		if (value->is_bool()) {
			out = value->get_bool();
		} else {
			warnings.push_back(bad_value(key, *value, "true or false"));
		}
	}

	struct file_read
	{
		bool exists = false;
		std::string text;
		std::string error;
	};

	file_read read_settings_file(const fs::path& file)
	{
		file_read out;
		std::error_code ec;
		if (!fs::exists(file, ec)) {
			if (ec) out.error = std::format("'{}' cannot be read: {}", utf8_of(file), ec.message());
			return out;
		}
		out.exists = true;
		std::ifstream stream(file, std::ios::binary);
		if (!stream) {
			out.error = std::format("'{}' cannot be read", utf8_of(file));
			return out;
		}
		out.text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		return out;
	}

	// The document of an existing file; an error for text that is not a JSON object.
	std::optional<std::string> parse_settings_document(const fs::path& file, const file_read& read, json::value& doc)
	{
		if (!read.error.empty()) return read.error;
		if (!read.exists || read.text.find_first_not_of(" \t\r\n") == std::string::npos) {
			doc = json::object{};
			return std::nullopt;
		}
		std::error_code ec;
		doc = json::parse(read.text, ec);
		if (ec) return std::format("'{}' is not valid JSON ({})", utf8_of(file), ec.message());
		if (!doc.is_object()) return std::format("'{}' is not a JSON object", utf8_of(file));
		return std::nullopt;
	}
}

std::string_view edt::jobs_mode_name(jobs_mode mode)
{
	return mode == jobs_mode::manual_jobs ? "manual" : "auto";
}

std::string_view edt::log_verbosity_name(log_verbosity verbosity)
{
	switch (verbosity) {
	case log_verbosity::errors: return "errors";
	case log_verbosity::normal: return "normal";
	case log_verbosity::verbose: return "verbose";
	}
	return "normal";
}

edt::parsed_build_settings edt::parse_build_settings(std::string_view json_text)
{
	parsed_build_settings out;
	std::error_code ec;
	const json::value doc = json::parse(json_text, ec);
	if (ec) {
		out.warnings.push_back(std::format("not valid JSON ({}): the defaults are used", ec.message()));
		return out;
	}
	if (!doc.is_object()) {
		out.warnings.push_back("not a JSON object: the defaults are used");
		return out;
	}
	const json::object& object = doc.get_object();
	build_settings& settings = out.settings;

	if (const json::value* value = object.if_contains(k_configuration); value && !value->is_null()) {
		if (value->is_string() && (value->get_string() == "debuggame" || value->get_string() == "release")) {
			settings.configuration = std::string(value->get_string());
		} else {
			out.warnings.push_back(bad_value(k_configuration, *value, "\"debuggame\" or \"release\""));
		}
	}
	if (const json::value* value = object.if_contains(k_jobs_mode); value && !value->is_null()) {
		if (value->is_string() && value->get_string() == "auto") {
			settings.jobs_mode = jobs_mode::auto_jobs;
		} else if (value->is_string() && value->get_string() == "manual") {
			settings.jobs_mode = jobs_mode::manual_jobs;
		} else {
			out.warnings.push_back(bad_value(k_jobs_mode, *value, "\"auto\" or \"manual\""));
		}
	}
	read_bool(object, k_reload_after_build, settings.reload_after_build, out.warnings);
	read_bool(object, k_build_before_play, settings.build_before_play, out.warnings);
	if (const json::value* value = object.if_contains(k_log_verbosity); value && !value->is_null()) {
		bool known = false;
		if (value->is_string()) {
			for (const log_verbosity candidate : { log_verbosity::errors, log_verbosity::normal, log_verbosity::verbose }) {
				if (value->get_string() == log_verbosity_name(candidate)) {
					settings.log_verbosity = candidate;
					known = true;
				}
			}
		}
		if (!known) out.warnings.push_back(bad_value(k_log_verbosity, *value, "\"errors\", \"normal\" or \"verbose\""));
	}
	if (const json::value* value = object.if_contains(k_fresh_configure); value && !value->is_null()) {
		if (!value->is_array()) {
			out.warnings.push_back(bad_value(k_fresh_configure, *value, "an array of configure preset names"));
		} else {
			// One bad entry drops only itself: the others still need their --fresh.
			for (const json::value& item : value->get_array()) {
				if (!item.is_string() || item.get_string().empty()) {
					out.warnings.push_back(std::format("'{}' holds {}, expected a configure preset name: it is ignored",
						k_fresh_configure, json::serialize(item)));
					continue;
				}
				std::string name(item.get_string());
				if (std::find(settings.fresh_configure.begin(), settings.fresh_configure.end(), name) == settings.fresh_configure.end()) {
					settings.fresh_configure.push_back(std::move(name));
				}
			}
		}
	}
	return out;
}

void edt::merge_build_settings(json::value& doc, const build_settings& settings)
{
	if (!doc.is_object()) doc = json::object{};
	json::object& object = doc.get_object();
	if (settings.configuration) {
		object[k_configuration] = *settings.configuration;
	} else {
		object.stable_erase(k_configuration);
	}
	object[k_jobs_mode] = jobs_mode_name(settings.jobs_mode);
	object[k_reload_after_build] = settings.reload_after_build;
	object[k_build_before_play] = settings.build_before_play;
	object[k_log_verbosity] = log_verbosity_name(settings.log_verbosity);
	if (settings.fresh_configure.empty()) {
		object.stable_erase(k_fresh_configure);
	} else {
		json::array names;
		for (const auto& name : settings.fresh_configure) names.emplace_back(json::string(name));
		object[k_fresh_configure] = std::move(names);
	}
}

edt::build_settings edt::load_build_settings(const fs::path& snk_dir)
{
	const fs::path file = snk_dir / k_build_settings_file;
	std::error_code ec;
	if (!fs::exists(file, ec)) return {};

	std::ifstream stream(file, std::ios::binary);
	if (!stream) {
		egLOG_WARN("editor/build", "'{}' cannot be read: the default build settings are used", utf8_of(file));
		return {};
	}
	const std::string text{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
	parsed_build_settings parsed = parse_build_settings(text);
	for (const auto& warning : parsed.warnings) {
		egLOG_WARN("editor/build", "'{}': {}", utf8_of(file), warning);
	}
	return parsed.settings;
}

std::optional<std::string> edt::check_build_settings_file(const fs::path& snk_dir)
{
	const fs::path file = snk_dir / k_build_settings_file;
	json::value doc;
	return parse_settings_document(file, read_settings_file(file), doc);
}

std::optional<std::string> edt::update_build_settings(const fs::path& snk_dir, const std::function<void(build_settings&)>& change)
{
	const fs::path file = snk_dir / k_build_settings_file;
	const file_read read = read_settings_file(file);
	json::value doc;
	if (auto problem = parse_settings_document(file, read, doc)) return problem;

	const build_settings before = read.exists ? parse_build_settings(read.text).settings : build_settings{};
	build_settings after = before;
	change(after);
	if (after == before) return std::nullopt;

	merge_build_settings(doc, after);
	std::error_code ec;
	fs::create_directories(snk_dir, ec);
	if (ec) return std::format("'{}' cannot be created: {}", utf8_of(snk_dir), ec.message());
	if (auto problem = prj::write_text_atomically(file, prj::to_indented_json(doc))) return problem;
	return std::nullopt;
}

std::optional<std::string> edt::clear_fresh_configure(const fs::path& snk_dir, std::string_view configuration)
{
	return update_build_settings(snk_dir, [configuration](build_settings& settings) {
		std::erase(settings.fresh_configure, configuration);
	});
}
