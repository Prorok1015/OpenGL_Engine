#include "edt_live_consent.h"
#include "edt_build_settings.h"
#include "edt_settings.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace json = boost::json;

namespace {

	constexpr std::string_view k_projects = "projects";

	json::object read_object(const std::optional<std::string>& text)
	{
		if (!text) return {};
		std::error_code ec;
		json::value doc = json::parse(*text, ec);
		if (ec || !doc.is_object()) return {};
		return std::move(doc.get_object());
	}

}

bool edt::live_consented(const std::optional<std::string>& settings_text, std::string_view project)
{
	const json::object root = read_object(settings_text);
	const json::value* projects = root.if_contains(k_projects);
	if (!projects || !projects->is_object()) return false;
	const json::value* entry = projects->get_object().if_contains(project);
	return entry && entry->is_bool() && entry->get_bool();
}

std::string edt::with_live_consent(const std::optional<std::string>& settings_text, std::string_view project,
	bool enabled)
{
	json::object root = read_object(settings_text);
	json::value& projects = root[k_projects];
	if (!projects.is_object()) projects = json::object{};
	if (enabled) {
		projects.get_object()[project] = true;
	} else {
		projects.get_object().erase(project);
	}
	return json::serialize(root);
}

bool edt::read_live_consent(res::resource_system& res, std::string_view project)
{
	return live_consented(read_setting(res, k_live_mode_settings), project);
}

std::optional<std::string> edt::store_live_consent(res::resource_system& res, std::string_view project, bool enabled)
{
	const std::string text = with_live_consent(read_setting(res, k_live_mode_settings), project, enabled);
	if (!write_setting(res, k_live_mode_settings, text)) {
		return std::format("edt://settings/{} cannot be written", k_live_mode_settings);
	}
	return std::nullopt;
}

bool edt::live_mode_at_open(const build_settings& project_settings, const std::optional<std::string>& settings_text,
	std::string_view project)
{
	const bool consented = live_consented(settings_text, project);
	if (project_settings.live_enabled && !consented) {
		egLOG_WARN("editor/build", "live_enabled in .snk/build.json is ignored: live mode builds and loads the "
			"project's code, so only Build > Live Mode turns it on, and it is kept in your own settings");
	}
	return consented;
}
