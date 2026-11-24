#include "edt_autosave_settings.h"
#include "../core/edt_settings.h"
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace json = boost::json;

namespace
{
	CFG_VAR_DEF_BOOL(cfg_edt_autosave, "editor/autosave", true);
}

edt::autosave_settings_result edt::parse_autosave_settings(std::string_view text)
{
	autosave_settings_result out;
	boost::system::error_code ec;
	const json::value root = json::parse(text, ec);
	if (ec || !root.is_object()) {
		out.error = "not a JSON object";
		return out;
	}
	autosave_settings parsed;
	for (const auto& member : root.as_object()) {
		const std::string_view name(member.key().data(), member.key().size());
		const json::value& value = member.value();
		if (name == "enabled") {
			if (!value.is_bool()) {
				out.error = "\"enabled\" must be true or false";
				return out;
			}
			parsed.enabled = value.as_bool();
		} else if (name == "interval_seconds") {
			if (!value.is_int64() && !value.is_uint64()) {
				out.error = "\"interval_seconds\" must be a whole number";
				return out;
			}
			const std::int64_t seconds = value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(std::min<std::uint64_t>(value.as_uint64(), k_autosave_max_interval));
			parsed.interval_seconds = static_cast<int>(std::clamp<std::int64_t>(seconds, k_autosave_min_interval, k_autosave_max_interval));
		} else {
			out.error = "unknown key '" + std::string(name) + "'";
			return out;
		}
	}
	out.settings = parsed;
	return out;
}

std::string edt::serialize_autosave_settings(const autosave_settings& settings)
{
	json::object root;
	root["enabled"] = settings.enabled;
	root["interval_seconds"] = static_cast<std::int64_t>(std::clamp(settings.interval_seconds, k_autosave_min_interval, k_autosave_max_interval));
	return prj::to_indented_json(root);
}

edt::autosave_settings edt::load_autosave_settings(res::resource_system& resources)
{
	const std::optional<std::string> text = read_setting(resources, k_autosave_settings_name);
	if (!text) {
		return {};
	}
	autosave_settings_result parsed = parse_autosave_settings(*text);
	if (!parsed.error.empty()) {
		egLOG_WARN("editor/autosave", "{}: {}; the defaults are used", settings_tag(k_autosave_settings_name).view(), parsed.error);
		return {};
	}
	return parsed.settings;
}

bool edt::save_autosave_settings(res::resource_system& resources, const autosave_settings& settings)
{
	return write_setting(resources, k_autosave_settings_name, serialize_autosave_settings(settings));
}

bool edt::autosave_allowed_by_config()
{
	return cfg_edt_autosave;
}
