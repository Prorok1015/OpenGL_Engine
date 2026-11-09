#include <snk/resource/res_shipping_dirs.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
	namespace fs = std::filesystem;

	constexpr std::size_t k_max_folder_name = 64;

	// The value as a path: wide on Windows, so a profile outside the ANSI code page is kept.
	fs::path environment_path(const char* name)
	{
#ifdef _WIN32
		const std::wstring wide_name{ name, name + std::strlen(name) };
		wchar_t* value = nullptr;
		std::size_t length = 0;
		if (_wdupenv_s(&value, &length, wide_name.c_str()) != 0 || value == nullptr) {
			return {};
		}
		fs::path out{ value };
		std::free(value);
		return out;
#else
		const char* value = std::getenv(name);
		return value != nullptr ? fs::path{ value } : fs::path{};
#endif
	}

	std::string scratch_prefix(std::string_view project_name)
	{
		return "snk-" + core::res::safe_folder_name(project_name) + "-";
	}

	bool is_digits(std::string_view text, std::size_t max)
	{
		if (text.empty() || text.size() > max) return false;
		for (const char c : text) {
			if (c < '0' || c > '9') return false;
		}
		return true;
	}

	bool is_hex(std::string_view text)
	{
		if (text.size() != 16) return false;
		for (const char c : text) {
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		}
		return true;
	}

	// "<pid>-<16 hex>" after the prefix: the pid, or -1 when the name is not a scratch folder's.
	std::int64_t scratch_pid(std::string_view rest)
	{
		const std::size_t dash = rest.find('-');
		if (dash == std::string_view::npos || !is_digits(rest.substr(0, dash), 18) || !is_hex(rest.substr(dash + 1))) {
			return -1;
		}
		return std::stoll(std::string{ rest.substr(0, dash) });
	}

	bool is_reserved_device_name(std::string_view name)
	{
		const std::string_view stem = name.substr(0, name.find('.'));
		std::string upper;
		for (const char c : stem) {
			upper.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
		}
		constexpr std::array<std::string_view, 4> plain{ "CON", "PRN", "AUX", "NUL" };
		for (const auto reserved : plain) {
			if (upper == reserved) return true;
		}
		if (!upper.starts_with("COM") && !upper.starts_with("LPT")) return false;
		const std::string_view digit = std::string_view{ upper }.substr(3);
		// COM1-9, and COM with a superscript 1, 2 or 3 (UTF-8 C2 B9, C2 B2, C2 B3), which Windows reserves too.
		return (digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9')
			|| digit == "\xC2\xB9" || digit == "\xC2\xB2" || digit == "\xC2\xB3";
	}

	// Length of the valid UTF-8 character at the start of `text` (shortest form, no surrogate, at most U+10FFFF); 0 when
	// it is not one.
	std::size_t utf8_character_length(std::string_view text)
	{
		const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
		const auto continuation = [&](std::size_t i) { return i < text.size() && (byte(i) & 0xC0) == 0x80; };
		const unsigned char lead = byte(0);
		if (lead < 0x80) return 1;
		if (lead >= 0xC2 && lead <= 0xDF) return continuation(1) ? 2 : 0;
		if (lead >= 0xE0 && lead <= 0xEF) {
			if (!continuation(1) || !continuation(2)) return 0;
			if (lead == 0xE0 && byte(1) < 0xA0) return 0;
			if (lead == 0xED && byte(1) >= 0xA0) return 0;
			return 3;
		}
		if (lead >= 0xF0 && lead <= 0xF4) {
			if (!continuation(1) || !continuation(2) || !continuation(3)) return 0;
			if (lead == 0xF0 && byte(1) < 0x90) return 0;
			if (lead == 0xF4 && byte(1) >= 0x90) return 0;
			return 4;
		}
		return 0;
	}

	void strip_trailing_dots_and_spaces(std::string& name)
	{
		// Windows drops them from a folder name: "Game." and "Game" would be one folder.
		while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
			name.pop_back();
		}
	}

	std::string random_tag()
	{
		std::random_device entropy;
		const std::uint64_t tag = (std::uint64_t{ entropy() } << 32) ^ std::uint64_t{ entropy() };
		return std::format("{:016x}", tag);
	}

	// POSIX: a real directory (lstat), owned by this user, mode 0700. Windows: a real directory, not a link or junction.
	bool is_private_directory(const fs::path& path)
	{
#ifdef _WIN32
		std::error_code ec;
		return fs::symlink_status(path, ec).type() == fs::file_type::directory && !ec;
#else
		struct stat info{};
		return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid()
			&& (info.st_mode & 07777) == 0700;
#endif
	}

	// The folder is made by this call or not at all: an existing one (planted, or a crashed run's) is never taken.
	bool create_new_private_directory(const fs::path& path, bool& exists, std::string& error)
	{
		exists = false;
#ifdef _WIN32
		if (CreateDirectoryW(path.c_str(), nullptr)) {
			return true;
		}
		const DWORD code = GetLastError();
		exists = code == ERROR_ALREADY_EXISTS;
		error = std::system_category().message(static_cast<int>(code));
		return false;
#else
		if (::mkdir(path.c_str(), 0700) != 0) {
			exists = errno == EEXIST;
			error = std::strerror(errno);
			return false;
		}
		// The umask may have taken bits off; nobody else can enter the folder in between (0700 at most).
		if (::chmod(path.c_str(), 0700) != 0) {
			error = std::strerror(errno);
			return false;
		}
		return true;
#endif
	}
}

std::string core::res::safe_folder_name(std::string_view name)
{
	std::string out;
	out.reserve(name.size());
	for (std::size_t i = 0; i < name.size();) {
		const char c = name[i];
		const std::size_t length = utf8_character_length(name.substr(i));
		if (length > 1) {
			// A letter outside ASCII stays, so "Игра" and "Мама" are two folders.
			out.append(name.substr(i, length));
			i += length;
			continue;
		}
		const bool plain = length == 1 && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == ' ' || c == '_' || c == '.' || c == '-');
		// An invalid byte becomes '_' too: path_from_utf8 would throw on it on Windows.
		out.push_back(plain ? c : '_');
		++i;
	}
	if (out.size() > k_max_folder_name) {
		std::size_t cut = k_max_folder_name;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
			--cut;
		}
		out.resize(cut);
	}
	strip_trailing_dots_and_spaces(out);
	if (out.empty() || out == "." || out == "..") {
		return "game";
	}
	if (is_reserved_device_name(out)) {
		out.push_back('_');
	}
	return out;
}

fs::path core::res::player_data_dir(std::string_view project_name)
{
	const fs::path folder = fs_paths::path_from_utf8(safe_folder_name(project_name));
#ifdef _WIN32
	const fs::path base = environment_path("LOCALAPPDATA");
	return base.empty() ? fs::path{} : base / folder;
#else
	const fs::path xdg = environment_path("XDG_DATA_HOME");
	if (!xdg.empty() && xdg.is_absolute()) {
		return xdg / folder;
	}
	const fs::path home = environment_path("HOME");
	return home.empty() ? fs::path{} : home / ".local" / "share" / folder;
#endif
}

#ifndef _WIN32
namespace {
	// stat, not lstat: a link to the folder is fine, only the folder it leads to is checked.
	bool is_own_private_base(const fs::path& base)
	{
		struct stat info{};
		return ::stat(base.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid()
			&& (info.st_mode & 0022) == 0 && ::access(base.c_str(), W_OK | X_OK) == 0;
	}

	// Writable by others only with the sticky bit (as /tmp): else another user could rename our folder away and put
	// theirs in its place between mkdir and the check.
	bool is_safe_shared_base(const fs::path& base)
	{
		struct stat info{};
		return ::stat(base.c_str(), &info) == 0 && S_ISDIR(info.st_mode)
			&& ((info.st_mode & 0022) == 0 || (info.st_mode & S_ISVTX) != 0) && ::access(base.c_str(), W_OK | X_OK) == 0;
	}
}
#endif

fs::path core::res::scratch_base_dir(std::string& error)
{
#ifndef _WIN32
	const fs::path runtime = environment_path("XDG_RUNTIME_DIR");
	if (runtime.is_absolute() && is_own_private_base(runtime)) {
		return runtime;
	}
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	for (const fs::path& candidate : { ec ? fs::path{} : temp, fs::path{ "/tmp" } }) {
		if (!candidate.empty() && is_safe_shared_base(candidate)) {
			return candidate;
		}
	}
	error = std::format("no temp folder that others cannot replace folders in (TMPDIR '{}', /tmp)", fs_paths::utf8_of(temp));
	return {};
#else
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	if (ec || temp.empty()) {
		error = std::format("no temp folder: {}", ec ? ec.message() : std::string{ "empty" });
		return {};
	}
	return temp;
#endif
}

core::res::scratch_folder_result core::res::make_scratch_dir(std::string_view project_name)
{
	scratch_folder_result result;
	const fs::path base = scratch_base_dir(result.error);
	if (base.empty()) {
		return result;
	}
	const std::string prefix = scratch_prefix(project_name) + std::to_string(platform::current_process_id()) + "-";
	for (int attempt = 0; attempt < 16; ++attempt) {
		const fs::path candidate = base / fs_paths::path_from_utf8(prefix + random_tag());
		bool exists = false;
		std::string error;
		if (!create_new_private_directory(candidate, exists, error)) {
			if (exists) {
				continue;
			}
			result.error = std::format("cannot create '{}': {}", fs_paths::utf8_of(candidate), error);
			return result;
		}
		if (!is_private_directory(candidate)) {
			result.error = std::format("'{}' is not a private folder of this user after it was made", fs_paths::utf8_of(candidate));
			return result;
		}
		result.path = candidate;
		return result;
	}
	result.error = std::format("no free scratch folder name in '{}'", fs_paths::utf8_of(base));
	return result;
}

std::size_t core::res::remove_stale_scratch_dirs(std::string_view project_name)
{
	std::string base_error;
	const fs::path base = scratch_base_dir(base_error);
	if (base.empty()) {
		return 0;
	}
#ifdef _WIN32
	// A shared temp folder (C:\Windows\Temp for a service) holds other users' folders: only one of this user's profile
	// is cleaned.
	// Canonical: %TEMP% often names the profile by its 8.3 short name (C:\Users\RUNNER~1\...), %LOCALAPPDATA% by the long one.
	std::error_code profile_ec;
	std::error_code base_ec;
	const fs::path profile_data = fs::canonical(environment_path("LOCALAPPDATA"), profile_ec);
	const fs::path canonical_base = fs::canonical(base, base_ec);
	if (profile_ec || base_ec || !fs_paths::is_inside(profile_data, canonical_base)) {
		egLOG_DEBUG("resource", "no cleanup of ended runs: '{}' is not in this user's profile", fs_paths::utf8_of(base));
		return 0;
	}
#endif
	const std::string prefix = scratch_prefix(project_name);
	const std::int64_t own = platform::current_process_id();
	std::size_t removed = 0;
	std::error_code ec;
	for (fs::directory_iterator it{ base, fs::directory_options::skip_permission_denied, ec }, end; !ec && it != end; it.increment(ec)) {
		const std::string name = fs_paths::utf8_of(it->path().filename());
		if (!name.starts_with(prefix)) {
			continue;
		}
		const std::int64_t pid = scratch_pid(std::string_view{ name }.substr(prefix.size()));
		if (pid < 0 || pid == own || platform::process_is_running(pid)) {
			continue;
		}
		if (!is_private_directory(it->path())) {
			egLOG_DEBUG("resource", "left alone: '{}' is not a private folder of this user", fs_paths::utf8_of(it->path()));
			continue;
		}
		std::error_code remove_ec;
		fs::remove_all(it->path(), remove_ec);
		if (remove_ec) {
			egLOG_WARN("resource", "cannot remove the scratch folder of an ended run '{}': {}", fs_paths::utf8_of(it->path()),
				remove_ec.message());
		} else {
			++removed;
		}
	}
	return removed;
}
