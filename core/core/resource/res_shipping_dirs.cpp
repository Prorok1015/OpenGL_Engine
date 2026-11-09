#include <snk/resource/res_shipping_dirs.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <cstdlib>
#include <string>
#include <system_error>

namespace {
	namespace fs = std::filesystem;

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

	std::string scratch_prefix(std::string_view project_name)
	{
		return "snk-" + core::res::safe_folder_name(project_name) + "-";
	}

	// A process id, short enough for std::stoll.
	bool is_pid_text(std::string_view text)
	{
		if (text.empty() || text.size() > 18) return false;
		for (const char c : text) {
			if (c < '0' || c > '9') return false;
		}
		return true;
	}
}

std::string core::res::safe_folder_name(std::string_view name)
{
	std::string out;
	out.reserve(name.size());
	for (const char c : name) {
		const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == ' ' || c == '_' || c == '.' || c == '-';
		out.push_back(plain ? c : '_');
	}
	// Windows drops a trailing dot or space from a folder name: "Game." and "Game" would be one folder.
	while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
		out.pop_back();
	}
	return out.empty() || out == "." || out == ".." ? std::string{ "game" } : out;
}

fs::path core::res::player_data_dir(std::string_view project_name)
{
	const std::string folder = safe_folder_name(project_name);
#ifdef _WIN32
	const std::string base = environment_value("LOCALAPPDATA");
	return base.empty() ? fs::path{} : fs::path{ base } / folder;
#else
	const std::string xdg = environment_value("XDG_DATA_HOME");
	if (!xdg.empty() && fs::path{ xdg }.is_absolute()) {
		return fs::path{ xdg } / folder;
	}
	const std::string home = environment_value("HOME");
	return home.empty() ? fs::path{} : fs::path{ home } / ".local" / "share" / folder;
#endif
}

fs::path core::res::scratch_dir_for(std::string_view project_name, std::int64_t pid)
{
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	return temp / (scratch_prefix(project_name) + std::to_string(pid));
}

std::size_t core::res::remove_stale_scratch_dirs(std::string_view project_name)
{
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	if (ec) {
		return 0;
	}
	const std::string prefix = scratch_prefix(project_name);
	const std::int64_t own = platform::current_process_id();
	std::size_t removed = 0;
	for (fs::directory_iterator it{ temp, fs::directory_options::skip_permission_denied, ec }, end; !ec && it != end; it.increment(ec)) {
		const std::string name = it->path().filename().string();
		if (!name.starts_with(prefix) || !is_pid_text(std::string_view{ name }.substr(prefix.size()))) {
			continue;
		}
		const std::int64_t pid = std::stoll(name.substr(prefix.size()));
		std::error_code type_ec;
		if (pid == own || it->is_symlink(type_ec) || !it->is_directory(type_ec) || platform::process_is_running(pid)) {
			continue;
		}
		std::error_code remove_ec;
		fs::remove_all(it->path(), remove_ec);
		if (remove_ec) {
			egLOG_WARN("resource", "cannot remove the scratch folder of an ended run '{}': {}", it->path().string(), remove_ec.message());
		} else {
			++removed;
		}
	}
	return removed;
}
