#include "edt_agent_source_rule.h"
#include "edt_agent_log_filter.h"
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <algorithm>
#include <array>
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

	std::string windows_volume_problem(std::string_view path, std::span<const std::string_view> parts,
		const edt::source_volume_ports& ports)
	{
		// Win32 strips a trailing space or dot: `x ` would open `x`, so the entry walked and the one read could differ.
		const auto stripped = [](std::string_view part) { return part.back() == ' ' || part.back() == '.'; };
		if (std::ranges::any_of(parts, stripped)) {
			return "a component of the source path ends in a space or a dot: name the file by its plain absolute path";
		}
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

	// The file systems of a local disk (or of memory): an allow-list, so a network, automount, FUSE or system one
	// (proc, sysfs, devtmpfs...) and any type not known here is refused.
	constexpr std::array<std::string_view, 14> k_local_mount_types = { "ext2", "ext3", "ext4", "xfs", "btrfs", "zfs",
		"f2fs", "vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "tmpfs", "overlay" };

	std::string posix_volume_problem(std::span<const std::string_view> parts, const edt::source_volume_ports& ports)
	{
		std::string normal;
		for (const std::string_view part : parts) normal += "/" + std::string(part);
		if (normal.empty()) normal = "/";
		// /proc/self/root/... and the like reach any file of the system through the proc mount.
		if (normal == "/proc" || normal.starts_with("/proc/")) {
			return "the source is under /proc: an agent may import only from a local disk";
		}
		const std::optional<std::string> table = ports.mount_table ? ports.mount_table() : std::nullopt;
		if (!table) return "the mount table cannot be read: an agent's source cannot be told to be on a local disk";
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
		if (std::ranges::find(k_local_mount_types, holder->type) == k_local_mount_types.end()) {
			return std::format("the source is on a '{}' file system, which is no local disk one (a network, automount, "
				"FUSE or system file system): an agent may import only from a local disk",
				text_escape::escaped_text(holder->type, 64));
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

	// The spellings of `path` a message may hold, longest first, without a trailing separator. `resolve`: also the
	// path as the disk resolves it, which follows every link on the way.
	std::vector<std::string> spellings_of(const fs::path& path, bool resolve)
	{
		std::vector<std::string> bases{ fs_paths::utf8_of(path), fs_paths::generic_utf8_of(path) };
		std::error_code ec;
		const fs::path resolved = resolve ? fs::weakly_canonical(path, ec) : fs::path{};
		if (resolve && !ec) {
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

std::optional<std::uint32_t> edt::system_file_attributes(const fs::path& path)
{
#if defined(_WIN32)
	// By name, no handle of ours: Windows reports the entry itself (a link's own attributes) and reads no data, so a
	// placeholder is not fetched.
	const DWORD attributes = GetFileAttributesW(platform::beneath::long_path(path).c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) return std::nullopt;
	return static_cast<std::uint32_t>(attributes);
#else
	static_cast<void>(path);
	return std::nullopt;
#endif
}

std::string edt::cloud_attributes_problem(std::uint32_t attributes)
{
	constexpr std::uint32_t fetched = k_file_attribute_offline | k_file_attribute_recall_on_open
		| k_file_attribute_recall_on_data_access;
	if ((attributes & fetched) == 0) return {};
	return "it is a cloud file that is not on this disk (online-only): opening it would download it; make the file "
		"available offline (keep it on this device) and ask again";
}

std::string edt::agent_source_links_problem(const fs::path& path, const source_volume_ports& ports,
	const fs::path& checked)
{
	if (!path.is_absolute()) return "the source must be an absolute path";
	if (!ports.entry_of) return "the entries on the way to the source cannot be inspected";
	// The parts of `checked` that `path` starts with were walked already.
	const fs::path relative = path.relative_path();
	auto part = relative.begin();
	const auto end = relative.end();
	fs::path at = path.root_path();
	if (!checked.empty() && checked.root_path() == path.root_path()) {
		auto from = part;
		fs::path walked = at;
		bool under = true;
		for (const fs::path& folder : checked.relative_path()) {
			if (folder.empty()) continue;
			if (from == end || *from != folder) {
				under = false;
				break;
			}
			walked /= *from;
			++from;
		}
		if (under) {
			part = from;
			at = std::move(walked);
		}
	}
	for (; part != end; ++part) {
		if (part->empty()) continue;
		if (*part == "." || *part == "..") {
			return "the source path holds a '.' or '..' component: name the file by its plain absolute path";
		}
		at /= *part;
		const std::string name = text_escape::escaped_text(fs_paths::utf8_of(*part), 256);
		std::error_code ec;
		const std::optional<platform::beneath::link_entry> entry = ports.entry_of(at, ec);
		if (ec) return std::format("'{}' on the way to the source cannot be inspected", name);
		if (!entry) return {};
		// A POSIX mount point is a plain folder to lstat: the mount-table rule judges it (sound: no link led here).
		if (*entry == platform::beneath::link_entry::link) {
			return std::format("'{}' on the way to the source is a link (a symlink, a junction, a mount point or "
				"another reparse point): an agent's import never follows one; name the file by its real path", name);
		}
		if (ports.windows && ports.file_attributes) {
			const std::optional<std::uint32_t> attributes = ports.file_attributes(at);
			if (!attributes) return std::format("'{}' on the way to the source cannot be inspected", name);
			if (std::string why = cloud_attributes_problem(*attributes); !why.empty()) {
				return std::format("'{}' on the way to the source: {}", name, why);
			}
		}
	}
	return {};
}

std::string edt::agent_side_file_problem(const fs::path& candidate, const fs::path& model_folder,
	const source_volume_ports& ports)
{
	if (std::string why = agent_source_path_problem(fs_paths::utf8_of(candidate), ports); !why.empty()) {
		return "an agent may not read it: " + why;
	}
	if (std::string why = agent_source_links_problem(candidate, ports, model_folder); !why.empty()) {
		return "an agent may not read it: " + why;
	}
	return {};
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
	if (ports.windows) return windows_volume_problem(path, parts, ports);
	if (path.front() != '/') return "the source must be an absolute path";
	return posix_volume_problem(parts, ports);
}

std::string edt::agent_source_path_problem(std::string_view path)
{
	return agent_source_path_problem(path, source_volume_ports{});
}

std::string edt::without_paths(std::string text, const std::vector<fs::path>& roots, const fs::path& source)
{
	// Longest spelling first, whoever it belongs to: a source inside a root is named by its file name, a root inside
	// the source's folder still reads res://.
	std::vector<std::pair<std::string, std::string>> replacements;
	if (!source.empty()) {
		const std::string name = fs_paths::utf8_of(source.filename());
		// The source is the resolved path of a plan: its text is all there is to replace, and the disk is not asked
		// (a link swapped in since would be followed).
		for (std::string& spelling : spellings_of(source, false)) {
			replacements.emplace_back(std::move(spelling), name);
		}
		// A decoder's error may name a side file beside the source: its folder is no business of an agent's either.
		// A root ("/", "C:/") is left: every path would lose its first separator.
		const fs::path folder = source.parent_path();
		if (folder.has_relative_path()) {
			for (std::string& spelling : spellings_of(folder, false)) {
				replacements.emplace_back(std::move(spelling), std::string(k_source_folder_text));
			}
		}
	}
	for (const fs::path& root : roots) {
		if (root.empty()) continue;
		for (const std::string& spelling : spellings_of(root, true)) {
			replacements.emplace_back(spelling + "/", "res://");
			replacements.emplace_back(spelling + "\\", "res://");
			replacements.emplace_back(spelling, "res://");
		}
	}
	const auto longer = [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); };
	std::ranges::stable_sort(replacements, longer);
	for (const auto& [from, to] : replacements) replace_all(text, from, to);
	return text;
}
