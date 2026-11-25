#include "edt_modeller_settings.h"
#include "../core/edt_settings.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cmath>
#include <optional>

namespace json = boost::json;

namespace
{
	std::optional<float> number_of(const json::value& value)
	{
		if (value.is_double()) return static_cast<float>(value.as_double());
		if (value.is_int64()) return static_cast<float>(value.as_int64());
		if (value.is_uint64()) return static_cast<float>(value.as_uint64());
		return std::nullopt;
	}

	float clamp_step(float value, float high, float fallback)
	{
		return std::isfinite(value) ? std::clamp(value, edt::k_modeller_min_step, high) : fallback;
	}
}

edt::modeller_settings edt::clamped(const modeller_settings& settings)
{
	modeller_settings out;
	const modeller_settings defaults;
	out.move_step = clamp_step(settings.move_step, k_modeller_max_step, defaults.move_step);
	out.rotate_step_degrees = clamp_step(settings.rotate_step_degrees, k_modeller_max_rotate_step, defaults.rotate_step_degrees);
	out.scale_step = clamp_step(settings.scale_step, k_modeller_max_step, defaults.scale_step);
	return out;
}

edt::modeller_settings_result edt::parse_modeller_settings(std::string_view text)
{
	modeller_settings_result out;
	boost::system::error_code ec;
	const json::value root = json::parse(text, ec);
	if (ec || !root.is_object()) {
		out.error = "not a JSON object";
		return out;
	}
	modeller_settings parsed;
	for (const auto& member : root.as_object()) {
		const std::string_view name(member.key().data(), member.key().size());
		float* target = nullptr;
		if (name == "move_step") target = &parsed.move_step;
		else if (name == "rotate_step_degrees") target = &parsed.rotate_step_degrees;
		else if (name == "scale_step") target = &parsed.scale_step;
		if (!target) {
			out.error = "unknown key '" + std::string(name) + "'";
			return out;
		}
		const std::optional<float> number = number_of(member.value());
		if (!number) {
			out.error = "\"" + std::string(name) + "\" must be a number";
			return out;
		}
		*target = *number;
	}
	out.settings = clamped(parsed);
	return out;
}

std::string edt::serialize_modeller_settings(const modeller_settings& settings)
{
	const modeller_settings safe = clamped(settings);
	json::object root;
	root["move_step"] = static_cast<double>(safe.move_step);
	root["rotate_step_degrees"] = static_cast<double>(safe.rotate_step_degrees);
	root["scale_step"] = static_cast<double>(safe.scale_step);
	return prj::to_indented_json(root);
}

edt::modeller_settings edt::load_modeller_settings(res::resource_system& resources)
{
	const std::optional<std::string> text = read_setting(resources, k_modeller_settings_name);
	if (!text) return {};
	modeller_settings_result parsed = parse_modeller_settings(*text);
	if (!parsed.error.empty()) {
		egLOG_WARN("editor/modeller", "{}: {}; the defaults are used", settings_tag(k_modeller_settings_name).view(), parsed.error);
		return {};
	}
	return parsed.settings;
}

bool edt::save_modeller_settings(res::resource_system& resources, const modeller_settings& settings)
{
	return write_setting(resources, k_modeller_settings_name, serialize_modeller_settings(settings));
}
