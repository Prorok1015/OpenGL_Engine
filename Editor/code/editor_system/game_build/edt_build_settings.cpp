#include "edt_build_settings.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace {
	constexpr std::string_view k_configuration = "configuration";
	constexpr std::string_view k_jobs_mode = "jobs_mode";
	constexpr std::string_view k_reload_after_build = "reload_after_build";
	constexpr std::string_view k_build_before_play = "build_before_play";
	constexpr std::string_view k_log_verbosity = "log_verbosity";

	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::string bad_value(std::string_view key, const json::value& value, std::string_view expected)
	{
		return std::format("'{}' is {}, expected {}: the default is used", key, json::serialize(value), expected);
	}

	void read_bool(const json::object& doc, std::string_view key, bool& out, std::vector<std::string>& warnings)
	{
		const json::value* value = doc.if_contains(key);
		if (!value) return;
		if (value->is_bool()) {
			out = value->get_bool();
		} else {
			warnings.push_back(bad_value(key, *value, "true or false"));
		}
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

	if (const json::value* value = object.if_contains(k_configuration)) {
		if (value->is_string() && (value->get_string() == "debuggame" || value->get_string() == "release")) {
			settings.configuration = std::string(value->get_string());
		} else if (!value->is_null()) {
			out.warnings.push_back(bad_value(k_configuration, *value, "\"debuggame\" or \"release\""));
		}
	}
	if (const json::value* value = object.if_contains(k_jobs_mode)) {
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
	if (const json::value* value = object.if_contains(k_log_verbosity)) {
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
