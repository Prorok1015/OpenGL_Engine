#include "edt_agent_source_rule.h"
#include "edt_agent_log_filter.h"
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
	bool is_separator(char c, bool windows)
	{
		return c == '/' || (windows && c == '\\');
	}

	// The components of `path` between separators, empty ones left out.
	std::vector<std::string_view> components_of(std::string_view path, bool windows)
	{
		std::vector<std::string_view> out;
		std::size_t begin = 0;
		for (std::size_t at = 0; at <= path.size(); ++at) {
			if (at < path.size() && !is_separator(path[at], windows)) continue;
			if (at > begin) out.push_back(path.substr(begin, at - begin));
			begin = at + 1;
		}
		return out;
	}

	std::string windows_volume_problem(std::string_view path, const edt::source_volume_ports& ports)
	{
		const char letter = path.size() >= 3 ? path[0] : '\0';
		const bool lettered = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
		if (!lettered || path[1] != ':' || !is_separator(path[2], true)) {
			return "the source must start with a drive letter (C:/...): an agent may import only from a local disk";
		}
		if (path.find(':', 2) != std::string_view::npos) return "the source path holds a ':' after its drive";
		// Only the drive's root, built from the letter: the drive table answers, the path is not looked at.
		const unsigned type = ports.drive_type ? ports.drive_type(letter) : 0;
		if (type != edt::k_drive_fixed && type != edt::k_drive_removable) {
			return std::format("the drive {}: is no local fixed or removable disk (a network drive, a CD, a RAM disk "
				"or none): an agent may import only from a local disk", letter);
		}
		return {};
	}

	std::string posix_volume_problem(std::span<const std::string_view> parts, const edt::source_volume_ports& ports)
	{
		const std::optional<std::string> table = ports.mount_table ? ports.mount_table() : std::nullopt;
		if (!table) return "the mount table cannot be read: an agent's source cannot be told to be on a local disk";
		std::string normal;
		for (const std::string_view part : parts) normal += "/" + std::string(part);
		if (normal.empty()) normal = "/";
		// The longest mount point on component boundaries holds the path; a later line over the same point wins.
		const edt::agent_log_mount* holder = nullptr;
		const std::vector<edt::agent_log_mount> mounts = edt::agent_log_mounts(*table);
		for (const edt::agent_log_mount& mount : mounts) {
			const std::string& point = mount.point;
			const bool under = point == "/" || normal == point
				|| (normal.size() > point.size() && normal.starts_with(point) && normal[point.size()] == '/');
			if (under && (holder == nullptr || point.size() >= holder->point.size())) holder = &mount;
		}
		if (holder == nullptr) return "no mount of the mount table holds the source";
		if (holder->remote) {
			return "the source is on a network or automount file system: an agent may import only from a local disk";
		}
		return {};
	}

	void replace_all(std::string& text, std::string_view from, std::string_view to)
	{
		if (from.empty()) return;
		for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
			text.replace(at, from.size(), to);
		}
	}

	// The spellings of `path` a message may hold, longest first, without a trailing separator.
	std::vector<std::string> spellings_of(const fs::path& path)
	{
		std::vector<std::string> bases{ fs_paths::utf8_of(path), fs_paths::generic_utf8_of(path) };
		std::error_code ec;
		const fs::path resolved = fs::weakly_canonical(path, ec);
		if (!ec) {
			bases.push_back(fs_paths::utf8_of(resolved));
			bases.push_back(fs_paths::generic_utf8_of(resolved));
		}
		std::vector<std::string> out;
		for (std::string base : bases) {
			while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) base.pop_back();
			if (base.empty()) continue;
			out.push_back("\\\\?\\" + base);
			out.push_back(std::move(base));
		}
		std::ranges::sort(out, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
		return out;
	}
}

#if defined(_WIN32)
static_assert(static_cast<unsigned>(DRIVE_FIXED) == edt::k_drive_fixed);
static_assert(static_cast<unsigned>(DRIVE_REMOVABLE) == edt::k_drive_removable);
#endif

unsigned edt::system_drive_type(char letter)
{
#if defined(_WIN32)
	const wchar_t root[4]{ static_cast<wchar_t>(letter), L':', L'\\', L'\0' };
	return static_cast<unsigned>(GetDriveTypeW(root));
#else
	static_cast<void>(letter);
	return 0;
#endif
}

std::optional<std::string> edt::system_mount_table()
{
#if defined(_WIN32)
	return std::nullopt;
#else
	std::ifstream in("/proc/self/mounts", std::ios::binary);
	if (!in) return std::nullopt;
	std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	if (text.empty()) return std::nullopt;
	return text;
#endif
}

std::string edt::agent_source_path_problem(std::string_view path, const source_volume_ports& ports)
{
	if (path.empty()) return "the source path is empty";
	const bool doubled = path.size() >= 2 && is_separator(path[0], true) && is_separator(path[1], true);
	if (path.starts_with("\\??\\") || doubled) {
		return "the source is a network share or a device path: an agent may import only from a local disk";
	}
	const std::vector<std::string_view> parts = components_of(path, ports.windows);
	if (std::ranges::any_of(parts, [](std::string_view part) { return part == "." || part == ".."; })) {
		return "the source path holds a '.' or '..' component: name the file by its plain absolute path";
	}
	if (ports.windows) return windows_volume_problem(path, ports);
	if (path.front() != '/') return "the source must be an absolute path";
	return posix_volume_problem(parts, ports);
}

std::string edt::agent_source_path_problem(std::string_view path)
{
	return agent_source_path_problem(path, source_volume_ports{});
}

std::string edt::without_paths(std::string text, const std::vector<fs::path>& roots, const fs::path& source)
{
	if (!source.empty()) {
		const std::string name = fs_paths::utf8_of(source.filename());
		for (const std::string& spelling : spellings_of(source)) replace_all(text, spelling, name);
	}
	for (const fs::path& root : roots) {
		if (root.empty()) continue;
		for (const std::string& spelling : spellings_of(root)) {
			replace_all(text, spelling + "/", "res://");
			replace_all(text, spelling + "\\", "res://");
			replace_all(text, spelling, "res://");
		}
	}
	return text;
}
