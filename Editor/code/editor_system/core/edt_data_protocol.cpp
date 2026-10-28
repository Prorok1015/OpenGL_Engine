#include "edt_data_protocol.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/resolvers/res_write_policy.h>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace {

	std::string environment_value(const char* name)
	{
#ifdef _WIN32
		char* value = nullptr;
		std::size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
			return {};
		}
		std::string out{ value };
		std::free(value);
		return out;
#else
		const char* value = std::getenv(name);
		return value != nullptr ? std::string{ value } : std::string{};
#endif
	}

}

std::filesystem::path edt::default_user_data_dir()
{
#ifdef _WIN32
	const std::string base = environment_value("APPDATA");
	if (!base.empty()) {
		return std::filesystem::path{ base } / "Snake" / "Editor";
	}
#else
	const std::string data_home = environment_value("XDG_DATA_HOME");
	if (!data_home.empty() && std::filesystem::path{ data_home }.is_absolute()) {
		return std::filesystem::path{ data_home } / "snake" / "editor";
	}
	const std::string home = environment_value("HOME");
	if (!home.empty()) {
		return std::filesystem::path{ home } / ".local" / "share" / "snake" / "editor";
	}
#endif
	std::error_code ec;
	return std::filesystem::temp_directory_path(ec) / "snake_editor";
}

std::vector<std::string> edt::default_write_patterns()
{
	return { "edt://settings/**" };
}

std::vector<std::string> edt::parse_write_patterns(std::string_view text)
{
	std::vector<std::string> out;
	while (!text.empty()) {
		const std::size_t end = text.find(';');
		std::string_view token = text.substr(0, end);
		const std::size_t first = token.find_first_not_of(" \t");
		if (first != std::string_view::npos) {
			out.emplace_back(token.substr(first, token.find_last_not_of(" \t") - first + 1));
		}
		text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
	}
	return out;
}

bool edt::register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path, const std::filesystem::path& user_path,
	const std::vector<std::string>& write_patterns)
{
	std::error_code ec;
	const bool exists = std::filesystem::is_directory(data_path, ec);
	if (!exists) {
		egLOG_WARN("editor/data", "editor data directory '{}' not found: shipped edt:// resources are unavailable", data_path.string());
	}

	// The vfs resolver reads top-down and stores into the top root only, so the shipped data stays untouched.
	auto& resolver = res.registrate_resolver<res::vfs_resolver>(DATA_PROTOCOL, std::vector<std::filesystem::path>{ data_path, user_path },
		res::mount_access::read_write, res::write_policy{ DATA_PROTOCOL, write_patterns });
	resolver.set_resource_changed_callback([&res](const res::tag& tag) { res.signal_changed(tag); });
	return exists;
}
