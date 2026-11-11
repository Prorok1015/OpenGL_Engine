#include <snk/core/core_log_location.h>
#include <cstdlib>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

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

	bool is_under(const fs::path& root, const fs::path& path)
	{
		const fs::path relative = path.lexically_relative(root);
		if (relative.empty() || relative == ".") {
			return false;
		}
		return *relative.begin() != "..";
	}
}

fs::path core::user_log_root_for(bool windows, std::string_view local_app_data, std::string_view xdg_state_home, std::string_view home)
{
	if (windows) {
		return local_app_data.empty() ? fs::path{} : fs::path{ local_app_data } / "Snake";
	}
	if (const fs::path state{ xdg_state_home }; !xdg_state_home.empty() && state.is_absolute()) {
		return state / "snake";
	}
	return home.empty() ? fs::path{} : fs::path{ home } / ".local" / "state" / "snake";
}

fs::path core::user_log_root()
{
#ifdef _WIN32
	return user_log_root_for(true, environment_value("LOCALAPPDATA"), {}, {});
#else
	return user_log_root_for(false, {}, environment_value("XDG_STATE_HOME"), environment_value("HOME"));
#endif
}

fs::path core::resolve_log_path(std::string_view configured, const fs::path& root)
{
	if (configured.empty() || root.empty()) {
		return {};
	}
	const fs::path wanted{ configured };
	if (wanted.is_absolute()) {
		return wanted;
	}
	const fs::path joined = (root / wanted).lexically_normal();
	return is_under(root.lexically_normal(), joined) ? joined : fs::path{};
}

std::string core::log_path_problem(const fs::path& trusted_root, const fs::path& file)
{
	if (trusted_root.empty()) {
		return {};
	}
	const fs::path root = trusted_root.lexically_normal();
	const fs::path target = file.lexically_normal();
	if (!is_under(root, target)) {
		return {};
	}
	std::error_code ec;
	fs::path current = root;
	for (const fs::path& part : target.lexically_relative(root)) {
		current /= part;
		const fs::file_status status = fs::symlink_status(current, ec);
		if (ec || !fs::exists(status)) {
			return {};
		}
		if (fs::is_symlink(status)) {
			return "'" + current.string() + "' is a link";
		}
		if (current != target && !fs::is_directory(status)) {
			return "'" + current.string() + "' is not a folder";
		}
	}
	return {};
}
