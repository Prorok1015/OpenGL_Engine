#include "edt_agent_log_filter.h"
#include "edt_agent_policy.h"
#include "edt_environment.h"
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
	namespace fs = std::filesystem;

	constexpr std::array<std::string_view, 5> k_hidden_categories{
		"edt/decoder", "capture", "mcp", "editor/agent_server", "editor/agent_policy",
	};
	constexpr std::string_view k_project_label = "<project>";
	constexpr std::string_view k_home_label = "<home>";
	constexpr std::string_view k_temp_label = "<tmp>";
	constexpr std::string_view k_any_label = "<path>";
	constexpr std::array<std::string_view, 4> k_labels{ k_project_label, k_home_label, k_temp_label, k_any_label };
#ifdef _WIN32
	constexpr std::array<const char*, 2> k_home_variables{ "USERPROFILE", "HOME" };
#else
	constexpr std::array<const char*, 1> k_home_variables{ "HOME" };
#endif

	bool is_or_below(std::string_view category, std::string_view parent)
	{
		return category == parent
			|| (category.size() > parent.size() && category.starts_with(parent) && category[parent.size()] == '/');
	}

	bool is_space(char c)
	{
		return std::isspace(static_cast<unsigned char>(c)) != 0;
	}

	bool is_letter(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	bool is_separator(char c)
	{
		return c == '/' || c == '\\';
	}

	// What a path is made of; a path starts after anything else. The bytes of a UTF-8 letter count as letters.
	bool is_path_char(char c)
	{
		const auto byte = static_cast<unsigned char>(c);
		return byte >= 0x80 || std::isalnum(byte) != 0 || c == '_' || c == '.' || c == '-' || is_separator(c);
	}

	bool ends_a_path(char c)
	{
		return is_space(c) || c == '\'' || c == '"' || c == ')' || c == ',' || c == ';' || c == '>' || c == ']' || c == '}' || c == '|';
	}

	bool is_digit(char c)
	{
		return c >= '0' && c <= '9';
	}

	// One token is looked at this far: a line without a space (a PATH list) is not scanned once per element.
	constexpr std::size_t k_token_limit = 4096;

	// `drive`: `from` is at "C:", whose ':' ends nothing.
	std::size_t token_end(std::string_view text, std::size_t from, bool drive = false)
	{
		const std::size_t limit = text.size() - from > k_token_limit ? from + k_token_limit : text.size();
		std::size_t end = drive && limit - from >= 2 ? from + 2 : from;
		while (end < limit && !ends_a_path(text[end])) {
			// "/a/b:/c/d" is a list: each element is a path of its own.
			if (text[end] == ':' && end + 1 < text.size() && text[end + 1] == '/') break;
			++end;
		}
		return end;
	}

	bool same_letter(char a, char b)
	{
#ifdef _WIN32
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
#else
		return a == b;
#endif
	}

	bool starts_a_token(std::string_view text, std::size_t at)
	{
		if (at == 0) return true;
		const char before = text[at - 1];
		if (is_path_char(before)) return false;
		if (before != '>') return true;
		// What follows a stand-in is the rest of a path that is already written.
		const std::string_view head = text.substr(0, at);
		return std::none_of(k_labels.begin(), k_labels.end(), [head](std::string_view label) { return head.ends_with(label); });
	}

	struct known_root
	{
		// With '/' and without a trailing one.
		std::string text;
		std::string_view label;
	};

	std::vector<known_root> known_roots(const edt::agent_log_roots& roots)
	{
		std::vector<known_root> out;
		const auto add = [&out](const std::vector<fs::path>& folders, std::string_view label) {
			for (const fs::path& folder : folders) {
				// "/" or "C:\" alone would be the root of every path.
				if (folder.relative_path().empty()) continue;
				std::string text = fs_paths::generic_utf8_of(folder);
				while (text.size() > 1 && text.back() == '/') text.pop_back();
				out.push_back(known_root{ std::move(text), label });
			}
		};
		add(roots.project, k_project_label);
		add(roots.temp, k_temp_label);
		add(roots.home, k_home_label);
		// The longest root first: a project inside the home folder is the project.
		std::stable_sort(out.begin(), out.end(), [](const known_root& a, const known_root& b) { return a.text.size() > b.text.size(); });
		return out;
	}

	// Whether `root` is at `from` as a whole folder name: "/p/demo" is not the root of "/p/demo2".
	bool root_matches(std::string_view text, std::size_t from, std::string_view root)
	{
		if (text.size() - from < root.size()) return false;
		for (std::size_t i = 0; i < root.size(); ++i) {
			const char have = text[from + i];
			if (root[i] == '/' ? !is_separator(have) : !same_letter(have, root[i])) return false;
		}
		const std::size_t after = from + root.size();
		if (after == text.size() || is_separator(text[after]) || !is_path_char(text[after])) return true;
		// The full stop of a sentence, not "demo.bak".
		return text[after] == '.' && (after + 1 == text.size() || !is_path_char(text[after + 1]));
	}

	bool is_drive(std::string_view rest)
	{
		return rest.size() >= 3 && is_letter(rest[0]) && rest[1] == ':' && is_separator(rest[2]);
	}

	// \\server\share and //server/share
	bool is_share(std::string_view rest)
	{
		if (rest.size() < 5 || !is_separator(rest[0]) || rest[1] != rest[0]) return false;
		std::size_t name_end = 2;
		while (name_end < rest.size() && is_path_char(rest[name_end]) && !is_separator(rest[name_end])) ++name_end;
		return name_end > 2 && name_end + 1 < rest.size() && is_separator(rest[name_end])
			&& is_path_char(rest[name_end + 1]) && !is_separator(rest[name_end + 1]);
	}

	// "\\?\" and "\\.\": what Windows puts before a path it takes as it is written.
	constexpr std::size_t k_long_prefix_length = 4;
	bool is_long_prefix(std::string_view rest)
	{
		return rest.starts_with("\\\\?\\") || rest.starts_with("\\\\.\\");
	}

	// "UNC\server\share" (what follows "\\?\" for a share).
	constexpr std::size_t k_long_share_length = 4;
	bool is_long_share(std::string_view rest)
	{
		if (rest.size() < k_long_share_length + 3 || !is_separator(rest[3])) return false;
		const auto is = [rest](std::size_t at, char lower) { return std::tolower(static_cast<unsigned char>(rest[at])) == lower; };
		if (!is(0, 'u') || !is(1, 'n') || !is(2, 'c')) return false;
		std::size_t name_end = k_long_share_length;
		while (name_end < rest.size() && is_path_char(rest[name_end]) && !is_separator(rest[name_end])) ++name_end;
		return name_end > k_long_share_length && name_end + 1 < rest.size() && is_separator(rest[name_end])
			&& is_path_char(rest[name_end + 1]) && !is_separator(rest[name_end + 1]);
	}

	// "/c/Users": the drive C: as MSYS and Git Bash write it. Elsewhere it is an absolute path like any other.
	bool is_msys_drive(std::string_view rest)
	{
#ifdef _WIN32
		return rest.size() >= 4 && rest[0] == '/' && is_letter(rest[1]) && rest[2] == '/' && is_path_char(rest[3]) && !is_separator(rest[3]);
#else
		static_cast<void>(rest);
		return false;
#endif
	}

	// The length of what `root` covers at `from` in the spelling found there; 0 when it is not at `from`.
	std::size_t root_length(std::string_view text, std::size_t from, std::string_view root, bool long_share, bool msys)
	{
		if (long_share) {
			// "UNC\server\share" is the root "//server/share".
			const bool found = root.starts_with("//") && root_matches(text, from + k_long_share_length, root.substr(2));
			return found ? root.size() - 2 + k_long_share_length : 0;
		}
		if (root_matches(text, from, root)) return root.size();
		// "/c/Users" is the root "C:/Users", "/z" the drive "Z:".
		const bool as_drive = msys && root.size() >= 2 && root[1] == ':' && same_letter(text[from + 1], root[0])
			&& root_matches(text, from + 2, root.substr(2));
		return as_drive ? root.size() : 0;
	}

	// The length of what is glued in front of a path: "-I" of "-I/usr/x", "/Fo" of "/FoC:\x", "\\?\" of "\\?\C:\x".
	std::size_t prefix_length(std::string_view rest)
	{
		if (is_long_prefix(rest)) return k_long_prefix_length;
		if (rest.empty() || (rest[0] != '-' && rest[0] != '/')) return 0;
		std::size_t letters = 1;
		while (letters < rest.size() && is_letter(rest[letters])) ++letters;
		if (letters == 1 || letters == rest.size()) return 0;
		if (is_drive(rest.substr(letters - 1))) return letters - 1;
		return rest[0] == '-' && rest[letters] == '/' ? letters : 0;
	}

	// Whether the word before `at` is a resource tag or a desc file: what follows one is a JSON pointer into it.
	bool follows_a_resource(std::string_view text, std::size_t at)
	{
		const auto between = [](char c) { return is_space(c) || c == ':' || c == ',' || c == '(' || c == '\'' || c == '"'; };
		std::size_t end = at;
		while (end > 0 && between(text[end - 1])) --end;
		std::size_t begin = end;
		while (begin > 0 && end - begin < k_token_limit && !is_space(text[begin - 1])) --begin;
		const std::string_view word = text.substr(begin, end - begin);
		return (word.find("://") != std::string_view::npos && !word.starts_with("file://")) || word.ends_with(".desc");
	}

	struct disk_match
	{
		bool found = false;
		// By a name of two segments, not only by a first segment whose content is not known.
		bool listed = false;
		// What the name covers in the text ("/media/John Smith", the space with it); 0 when not `listed`.
		std::size_t length = 0;
	};

	// A character as names are ordered and looked up by: without its case where same_letter ignores it.
	unsigned char folded(char c)
	{
#ifdef _WIN32
		return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
#else
		return static_cast<unsigned char>(c);
#endif
	}

	bool name_less(std::string_view a, std::string_view b)
	{
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return folded(x) < folded(y); });
	}

	// Where the first segment of a listed name ends, the '/' after it included: 5 of "/usr/bin", 3 of "C:/Users".
	std::size_t first_segment_length(std::string_view name)
	{
		const std::size_t slash = name.size() > 1 ? name.find('/', 1) : std::string_view::npos;
		return slash == std::string_view::npos ? 0 : slash + 1;
	}

	// The first segment of `name` against the start of `text`, where '\' is a separator too: below 0 when it sorts
	// before it, 0 when `text` starts with it. No first segment is the start of another, so names are ordered by it.
	int segment_order(std::string_view name, std::string_view text)
	{
		const std::size_t length = first_segment_length(name);
		for (std::size_t i = 0; i < length; ++i) {
			if (i == text.size()) return 1;
			const unsigned char have = folded(is_separator(text[i]) ? '/' : text[i]);
			const unsigned char want = folded(name[i]);
			if (want != have) return want < have ? -1 : 1;
		}
		return 0;
	}

	// What the listing of the disk says of the token at `from`. Compares text only: never asks the file system.
	disk_match disk_at(std::string_view text, std::size_t from, const edt::agent_log_disk& disk, bool msys)
	{
		disk_match out;
		const std::vector<std::string>& names = disk.names();
		const auto longest = [&](std::string_view start) {
			const auto before = [](const std::string& name, std::string_view key) { return segment_order(name, key) < 0; };
			auto name = std::lower_bound(names.begin(), names.end(), start, before);
			for (; name != names.end() && segment_order(*name, start) == 0; ++name) {
				const std::size_t covered = root_length(text, from, *name, false, msys);
				if (covered > out.length) out = disk_match{ true, true, covered };
			}
		};
		longest(text.substr(from));
		// "/c/Users" is listed as "C:/Users".
		if (msys) {
			const std::string drive{ text[from + 1], ':', '/' };
			longest(drive);
		}
		if (out.found) return out;
		if (disk.whole_root() && text[from] == '/' && from + 1 < text.size()) {
			out.found = is_path_char(text[from + 1]) && !is_separator(text[from + 1]);
			if (out.found) return out;
		}
		for (const std::string& name : disk.unlisted()) {
			const std::size_t covered = name.empty() ? 0 : root_length(text, from, name, false, msys);
			const std::size_t next = from + covered + 1;
			if (covered == 0 || next >= text.size() || !is_separator(text[next - 1])) continue;
			if (!is_path_char(text[next]) || is_separator(text[next])) continue;
			out.found = true;
			break;
		}
		return out;
	}

	// Where a path that ends at `last` at most (and not before `least`) ends: before the punctuation of a sentence
	// and before ":<line>" or ":<line>:<col>".
	std::size_t path_end(std::string_view text, std::size_t least, std::size_t last)
	{
		while (last > least && (text[last - 1] == '.' || text[last - 1] == ':')) --last;
		for (int part = 0; part < 2; ++part) {
			std::size_t digits = last;
			while (digits > least && is_digit(text[digits - 1])) --digits;
			if (digits == last || digits <= least || text[digits - 1] != ':') break;
			last = digits - 1;
		}
		return last;
	}

	struct found_path
	{
		// What the stand-in replaces; nothing when `length` is 0.
		std::size_t from = 0;
		std::size_t length = 0;
		std::string_view label;
		// What is left as it is after the stand-in (or from the place looked at, when there is none).
		std::size_t skip = 1;
	};

	found_path path_at(std::string_view text, std::size_t at, const std::vector<known_root>& known, const edt::agent_log_roots& roots)
	{
		std::size_t from = at;
		bool in_file_address = false;
		if (at > 0 && text[at - 1] == ':' && text.substr(at).starts_with("//")) {
			// Only file:// names a file; what follows another scheme is a host or a resource and stays.
			if (!text.substr(0, at).ends_with("file:")) return found_path{ at, 0, {}, token_end(text, at) - at };
			in_file_address = true;
			from = at + 2;
			if (from >= text.size() || ends_a_path(text[from])) return {};
			if (text[from] != '/') return found_path{ at, token_end(text, at) - at, k_any_label, 0 };
			if (is_drive(text.substr(from + 1))) ++from;
		} else {
			from = at + prefix_length(text.substr(at));
		}

		const std::string_view rest = text.substr(from);
		const bool long_form = !in_file_address && from != at && is_long_prefix(text.substr(at));
		const bool long_share = long_form && is_long_share(rest);
		const bool drive = is_drive(rest);
		const bool share = from == at && is_share(rest);
		const bool rooted = rest.size() >= 2 && rest[0] == '/' && !is_separator(rest[1]);
		if (!drive && !share && !rooted && !long_share) return {};
		const bool msys = rooted && !in_file_address && is_msys_drive(rest);
		// The stand-in takes "\\?\" with it: "\\?\C:\p\demo\a" reads "<project>\a".
		const std::size_t begin = long_form ? at : from;

		for (const known_root& root : known) {
			const std::size_t covered = root_length(text, from, root.text, long_share, msys);
			if (covered == 0) continue;
			const std::size_t after = from + covered;
			return found_path{ begin, after - begin, root.label, token_end(text, after) - after };
		}

		const disk_match on_disk = disk_at(text, from, roots.disk, msys);
		// "/header/type" may as well be a JSON pointer: a path only when the listing of the disk has it.
		if (rooted && !in_file_address) {
			if (!on_disk.found) return {};
			if (!on_disk.listed && follows_a_resource(text, at)) return {};
		}
		// A space is the path's only inside a folder the listing has ("/media/John Smith"); elsewhere it ends it.
		const std::size_t listed_end = from + on_disk.length;
		const std::size_t least = on_disk.length > 0 ? listed_end : from + 1;
		const std::size_t end = path_end(text, least, token_end(text, listed_end, drive && on_disk.length == 0));
		return found_path{ begin, end - begin, k_any_label, 0 };
	}

	void add_root(std::vector<fs::path>& out, const fs::path& folder)
	{
		if (folder.empty()) return;
		std::error_code ec;
		fs::path spelled = fs::absolute(folder, ec).lexically_normal();
		if (ec || spelled.empty()) return;
		if (!spelled.has_filename()) spelled = spelled.parent_path();
		out.push_back(spelled);
		// As the system names it: /private/tmp for /tmp on macOS, the target of a linked project folder.
		fs::path resolved = fs::weakly_canonical(spelled, ec);
		if (!ec && !resolved.empty() && resolved != spelled) out.push_back(std::move(resolved));
	}

	// The entries of `folder` as "<prefix>/<name>"; false when there are more than `most` or it cannot be read.
	bool list_folder(const fs::path& folder, const std::string& prefix, std::size_t most, std::vector<std::string>& out)
	{
		std::error_code ec;
		fs::directory_iterator entry(folder, ec);
		for (; !ec && entry != fs::directory_iterator(); entry.increment(ec)) {
			if (out.size() >= most) return false;
			out.push_back(prefix + "/" + fs_paths::utf8_of(entry->path().filename()));
		}
		return !ec;
	}

	// A listing while it is made; agent_log_disk orders it.
	struct disk_listing
	{
		std::vector<std::string> names;
		std::vector<std::string> unlisted;
		bool whole_root = false;
	};

	// Adds two levels of `root` to `out` under `prefix` ("" or "C:"); `with_first`: the first-level names too. Once
	// `limits.total` is used up no folder is asked or entered: each name left goes to `unlisted`.
	void list_disk(disk_listing& out, const fs::path& root, const std::string& prefix, std::span<const std::string> skipped,
		const edt::agent_log_disk_limits& limits, bool with_first)
	{
		const auto used = [&out] { return out.names.size() + out.unlisted.size(); };
		std::size_t unread = 0;
		std::error_code ec;
		fs::directory_iterator entry(root, ec);
		for (; !ec && entry != fs::directory_iterator(); entry.increment(ec)) {
			const std::string name = fs_paths::utf8_of(entry->path().filename());
			const std::string first = prefix + "/" + name;
			// Before anything asks the folder itself: a network mount may not answer.
			const bool read = used() < limits.total && std::find(skipped.begin(), skipped.end(), name) == skipped.end();
			if (read && with_first) out.names.push_back(first);
			std::error_code entry_ec;
			if (read && (!entry->is_directory(entry_ec) || entry_ec)) continue;
			std::vector<std::string> below;
			if (read && list_folder(entry->path(), first, limits.per_folder, below) && used() + below.size() <= limits.total) {
				out.names.insert(out.names.end(), std::make_move_iterator(below.begin()), std::make_move_iterator(below.end()));
				continue;
			}
			// A root with more unread folders than this counts as a path whole, so `unlisted` stays small and nothing is shown.
			if (unread++ == limits.per_folder) {
				if (prefix.empty()) {
					out.whole_root = true;
				} else {
					out.unlisted.push_back(prefix);
				}
				return;
			}
			out.unlisted.push_back(first);
		}
	}

	bool is_remote_mount_type(std::string_view type)
	{
		constexpr std::array<std::string_view, 8> k_remote{ "cifs", "smb3", "autofs", "9p", "ceph", "glusterfs", "davfs", "fuse" };
		// A FUSE file system may be a network one whatever its name (sshfs, glusterfs, rclone): none is entered.
		return type.starts_with("nfs") || type.starts_with("fuse.") || std::find(k_remote.begin(), k_remote.end(), type) != k_remote.end();
	}

	// "\040" of /proc/self/mounts is a space, "\011" a tab, "\012" a newline, "\134" a backslash.
	std::string unescaped_mount(std::string_view text)
	{
		std::string out;
		for (std::size_t at = 0; at < text.size(); ++at) {
			const auto octal = [text](std::size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '7'; };
			if (text[at] == '\\' && octal(at + 1) && text[at + 1] <= '3' && octal(at + 2) && octal(at + 3)) {
				out.push_back(static_cast<char>((text[at + 1] - '0') * 64 + (text[at + 2] - '0') * 8 + (text[at + 3] - '0')));
				at += 3;
			} else {
				out.push_back(text[at]);
			}
		}
		return out;
	}

	// The next field of a line of /proc/self/mounts, taken off `line`; empty when there is none.
	std::string_view next_mount_field(std::string_view& line)
	{
		const std::size_t begin = line.find_first_not_of(" \t\r");
		if (begin == std::string_view::npos) return {};
		line.remove_prefix(begin);
		const std::size_t space = line.find_first_of(" \t\r");
		const std::string_view field = line.substr(0, space);
		line.remove_prefix(field.size());
		return field;
	}

#ifdef _WIN32
	static_assert(static_cast<unsigned>(DRIVE_FIXED) == edt::k_agent_log_drive_fixed);

	// From the mount table of the system: asks no drive.
	unsigned drive_type_of(char letter)
	{
		const wchar_t root[4]{ static_cast<wchar_t>(letter), L':', L'\\', L'\0' };
		return static_cast<unsigned>(GetDriveTypeW(root));
	}

	// 0 when the current folder is on no drive (a share) or its name does not fit.
	char current_drive_letter()
	{
		wchar_t folder[MAX_PATH];
		const DWORD length = GetCurrentDirectoryW(MAX_PATH, folder);
		if (length < 2 || length >= MAX_PATH || folder[1] != L':' || folder[0] > L'z') return '\0';
		const char letter = static_cast<char>(folder[0]);
		return is_letter(letter) ? letter : '\0';
	}
#else
	// The first-level folders the listing does not enter: reading one can block (a server that is gone) or never end.
	std::vector<std::string> skipped_folders()
	{
		std::vector<std::string> out{ "net", "proc", "sys", "dev" };
		std::ifstream mounts("/proc/self/mounts");
		const std::string text{ std::istreambuf_iterator<char>(mounts), std::istreambuf_iterator<char>() };
		std::vector<std::string> remote = edt::agent_log_remote_mounts(text);
		out.insert(out.end(), std::make_move_iterator(remote.begin()), std::make_move_iterator(remote.end()));
		return out;
	}
#endif
}

edt::agent_log_disk::agent_log_disk(std::vector<std::string> names, std::vector<std::string> unlisted, bool whole_root)
	: m_unlisted(std::move(unlisted)), m_whole_root(whole_root)
{
	m_names.reserve(names.size());
	for (std::string& name : names) {
		const std::size_t length = first_segment_length(name);
		if (length == 0 || length == name.size()) continue;
		const std::string_view segment = std::string_view(name).substr(0, length - 1);
		// The lookup reads '\' as a separator, so such a segment has no place in the order: all below it is a path.
		if (segment.find('\\') != std::string_view::npos) {
			m_unlisted.emplace_back(segment);
			continue;
		}
		m_names.push_back(std::move(name));
	}
	std::sort(m_names.begin(), m_names.end(), [](const std::string& a, const std::string& b) { return name_less(a, b); });
}

edt::agent_log_disk edt::agent_log_disk_of(const fs::path& root, std::span<const std::string> skipped, agent_log_disk_limits limits)
{
	disk_listing found;
	list_disk(found, root, {}, skipped, limits, false);
	return agent_log_disk(std::move(found.names), std::move(found.unlisted), found.whole_root);
}

bool edt::agent_log_drive_is_listed(char letter, unsigned type)
{
	// A: and B: are floppy drives whatever they report: asking one without a disk in it can take seconds.
	const bool floppy = letter == 'A' || letter == 'B' || letter == 'a' || letter == 'b';
	return is_letter(letter) && !floppy && type == k_agent_log_drive_fixed;
}

std::vector<edt::agent_log_mount> edt::agent_log_mounts(std::string_view mounts)
{
	std::vector<agent_log_mount> out;
	while (!mounts.empty()) {
		const std::size_t line_end = mounts.find('\n');
		std::string_view line = mounts.substr(0, line_end);
		mounts.remove_prefix(line_end == std::string_view::npos ? mounts.size() : line_end + 1);
		next_mount_field(line);
		std::string point = unescaped_mount(next_mount_field(line));
		const std::string_view type = next_mount_field(line);
		if (point.empty() || point[0] != '/') continue;
		out.push_back({ std::move(point), is_remote_mount_type(type) });
	}
	return out;
}

std::vector<std::string> edt::agent_log_remote_mounts(std::string_view mounts)
{
	std::vector<std::string> out;
	for (const agent_log_mount& mount : agent_log_mounts(mounts)) {
		const std::string& point = mount.point;
		// The listing enters first-level folders only: a mount point further down is never read.
		if (!mount.remote || point.size() < 2 || point.find('/', 1) != std::string::npos) continue;
		out.push_back(point.substr(1));
	}
	return out;
}

edt::agent_log_disk edt::agent_log_disk_at_start()
{
	disk_listing found;
	const agent_log_disk_limits limits{};
#ifdef _WIN32
	// "/Users/x" is a path on the current drive: listed only when that drive is a local disk.
	const char current = current_drive_letter();
	if (current != '\0' && agent_log_drive_is_listed(current, drive_type_of(current))) {
		list_disk(found, fs::path("/"), {}, {}, limits, false);
	} else {
		// Nothing tells a path on a drive that is not read from a JSON pointer: every rooted token is a path.
		found.whole_root = true;
	}
	const DWORD drives = GetLogicalDrives();
	for (int index = 0; index < 26; ++index) {
		if ((drives & (DWORD{ 1 } << index)) == 0) continue;
		const char letter = static_cast<char>('A' + index);
		const std::string drive{ letter, ':' };
		const bool room = found.names.size() + found.unlisted.size() < limits.total;
		if (room && agent_log_drive_is_listed(letter, drive_type_of(letter))) {
			list_disk(found, fs_paths::path_from_utf8(drive + "/"), drive, {}, limits, true);
		} else {
			found.unlisted.push_back(drive);
		}
	}
#else
	const std::vector<std::string> skipped = skipped_folders();
	list_disk(found, fs::path("/"), {}, skipped, limits, false);
#endif
	return agent_log_disk(std::move(found.names), std::move(found.unlisted), found.whole_root);
}

edt::agent_log_roots edt::agent_log_roots_of(const fs::path& project)
{
	agent_log_roots roots;
	roots.disk = agent_log_disk_at_start();
	add_root(roots.project, project);
	for (const char* variable : k_home_variables) {
		if (const auto home = environment_utf8(variable); home && !home->empty()) {
			add_root(roots.home, fs_paths::path_from_utf8(*home));
			break;
		}
	}
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	if (!ec) add_root(roots.temp, temp);
	return roots;
}

bool edt::agent_log_category_visible(std::string_view category, bool build_allowed)
{
	if (!build_allowed && is_or_below(category, k_agent_build_category)) return false;
	return std::none_of(k_hidden_categories.begin(), k_hidden_categories.end(),
		[category](std::string_view hidden) { return is_or_below(category, hidden); });
}

std::string edt::agent_log_text(std::string_view text, const agent_log_roots& roots)
{
	const std::vector<known_root> known = known_roots(roots);
	std::string out(text);
	for (std::size_t at = 0; at < out.size();) {
		const found_path found = starts_a_token(out, at) ? path_at(out, at, known, roots) : found_path{};
		if (found.length == 0) {
			at += found.skip;
			continue;
		}
		// The label is a literal: it stays valid while `out` changes under it.
		out.replace(found.from, found.length, found.label);
		at = found.from + found.label.size() + found.skip;
	}
	return out;
}

bool edt::agent_build_output_allowed(agent_policy& policy, const cmd::command_registry& commands)
{
	const cmd::command_desc* build = commands.find("build.game_module");
	return build != nullptr && policy.effective(*build).mode == cmd::policy_mode::allow;
}

void edt::install_agent_log_filter(cmd::log_read_filter& filter, std::function<bool()> build_allowed, agent_log_roots roots)
{
	// Asked for every entry of a read: the policy (which looks at its files) is asked a few times a second at most.
	struct build_answer
	{
		bool asked = false;
		bool allowed = false;
		std::chrono::steady_clock::time_point at{};
	};
	filter.visible = [build_allowed = std::move(build_allowed), answer = std::make_shared<build_answer>()](std::string_view category, const cmd::origin& from) {
		if (from.kind == cmd::origin_kind::ui) return true;
		const auto now = std::chrono::steady_clock::now();
		if (!answer->asked || now - answer->at > std::chrono::milliseconds(250)) {
			answer->asked = true;
			answer->allowed = build_allowed && build_allowed();
			answer->at = now;
		}
		return agent_log_category_visible(category, answer->allowed);
	};
	filter.redact = [roots = std::move(roots)](std::string_view text, const cmd::origin& from) {
		return from.kind == cmd::origin_kind::ui ? std::string(text) : agent_log_text(text, roots);
	};
}
