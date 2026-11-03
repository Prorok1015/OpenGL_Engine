#include "edt_agent_server_settings.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <fstream>
#include <iterator>
#include <system_error>

namespace json = boost::json;

namespace edt
{
	agent_server_settings_result parse_agent_server_settings(std::string_view text)
	{
		agent_server_settings_result out;
		boost::system::error_code ec;
		const json::value root = json::parse(text, ec);
		if (ec || !root.is_object()) {
			out.error = "not a JSON object";
			return out;
		}
		agent_server_settings parsed;
		for (const auto& member : root.as_object()) {
			const std::string_view name(member.key().data(), member.key().size());
			const json::value& value = member.value();
			if (name == "enabled") {
				if (!value.is_bool()) {
					out.error = "\"enabled\" must be true or false";
					return out;
				}
				parsed.enabled = value.as_bool();
			}
			else if (name == "port") {
				if (!value.is_int64() || value.as_int64() < 0 || value.as_int64() > 65535) {
					out.error = "\"port\" must be a whole number from 0 to 65535";
					return out;
				}
				parsed.port = static_cast<std::uint16_t>(value.as_int64());
			}
			else {
				out.error = "unknown key '" + std::string(name) + "'";
				return out;
			}
		}
		out.settings = parsed;
		return out;
	}

	std::string serialize_agent_server_settings(const agent_server_settings& settings)
	{
		json::object root;
		root["enabled"] = settings.enabled;
		root["port"] = static_cast<std::int64_t>(settings.port);
		return prj::to_indented_json(root);
	}

	agent_server_settings load_agent_server_settings(const std::filesystem::path& file)
	{
		std::error_code ec;
		if (file.empty() || !std::filesystem::exists(file, ec)) {
			return {};
		}
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			egLOG_WARN("editor/agent_server", "{}: the file cannot be read; the agent server stays off", fs_paths::utf8_of(file));
			return {};
		}
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		agent_server_settings_result parsed = parse_agent_server_settings(text);
		if (!parsed.error.empty()) {
			egLOG_WARN("editor/agent_server", "{}: {}; the agent server stays off", fs_paths::utf8_of(file), parsed.error);
			return {};
		}
		return parsed.settings;
	}

	std::optional<std::string> save_agent_server_settings(const std::filesystem::path& file, const agent_server_settings& settings)
	{
		if (!file.has_parent_path()) {
			return "no place to write the settings";
		}
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		return prj::write_text_atomically(file, serialize_agent_server_settings(settings));
	}
}
