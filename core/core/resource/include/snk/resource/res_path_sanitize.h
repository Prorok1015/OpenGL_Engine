#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Helpers for names and paths that come from untrusted files (models, descs).
namespace res
{
	inline constexpr std::size_t max_sanitized_part_length = 255;
	inline constexpr std::size_t max_sanitized_path_length = 1024;
	inline constexpr std::uintmax_t max_external_file_bytes = 256ull * 1024 * 1024;

	inline bool has_control_chars(const std::string_view s)
	{
		for (const char c : s) {
			const auto u = static_cast<unsigned char>(c);
			if (u < 0x20 || u == 0x7f) {
				return true;
			}
		}
		return false;
	}

	// Why a relative path (UTF-8, '/' separated) is not a name every platform can hold, or empty when it is: a ':' (a
	// Windows alternate data stream or a drive), a control character, an empty segment, '.' or '..', a segment ending in
	// a space or a dot, or a Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension,
	// whatever the case). Checked on every platform so a project stays portable. Pure.
	inline std::string portable_path_problem(std::string_view relative)
	{
		if (relative.empty()) return "the path is empty";
		std::size_t begin = 0;
		while (begin <= relative.size()) {
			const std::size_t end = relative.find('/', begin);
			const std::string_view segment = relative.substr(begin, end == std::string_view::npos ? end : end - begin);
			if (segment.empty()) return "a name is empty";
			if (segment == "." || segment == "..") return "'.' and '..' are not allowed";
			if (has_control_chars(segment)) return "a name has a control character";
			if (segment.find(':') != std::string_view::npos) return "a name cannot hold ':' (a stream or a drive on Windows)";
			if (segment.back() == ' ' || segment.back() == '.') return "a name cannot end in a space or a dot";
			std::string stem{ segment.substr(0, segment.find('.')) };
			for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			while (!stem.empty() && stem.back() == ' ') stem.pop_back();
			const bool numbered = stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9';
			if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || numbered) {
				return "a name cannot be a device name of Windows (CON, NUL, COM1, ...)";
			}
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}
		return {};
	}

	namespace detail
	{
		// Cut at `limit` bytes without splitting a UTF-8 sequence.
		inline void truncate_utf8(std::string& s, const std::size_t limit)
		{
			if (s.size() <= limit) {
				return;
			}
			std::size_t cut = limit;
			while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
				--cut;
			}
			s.resize(cut);
		}
	}

	// Turns an untrusted file name (texture path, material or embedded file name from a model file) into a
	// safe relative path for building a tag: drops empty, "." and ".." parts, root and drive parts, replaces
	// ':' and control characters, caps part and total length, and never returns an empty string.
	// Backslashes are separators on every platform.
	inline std::string sanitize_relative_path(const std::string_view raw)
	{
		std::string out;
		out.reserve(raw.size() < max_sanitized_path_length ? raw.size() : max_sanitized_path_length);

		bool first = true;
		std::size_t pos = 0;
		while (pos <= raw.size()) {
			std::size_t end = raw.find_first_of("/\\", pos);
			if (end == std::string_view::npos) {
				end = raw.size();
			}
			std::string part{ raw.substr(pos, end - pos) };
			pos = end + 1;

			if (first && part.size() >= 2 && std::isalpha(static_cast<unsigned char>(part[0])) && part[1] == ':') {
				part.erase(0, 2);
			}
			first = false;

			if (part.empty() || part == "." || part == "..") {
				continue;
			}
			// ':' would let a part spell a protocol ("x://") or a drive.
			for (char& c : part) {
				const auto u = static_cast<unsigned char>(c);
				if (c == ':' || u < 0x20 || u == 0x7f) {
					c = '_';
				}
			}
			detail::truncate_utf8(part, max_sanitized_part_length);

			if (!out.empty()) {
				out += '/';
			}
			out += part;
		}

		detail::truncate_utf8(out, max_sanitized_path_length);
		while (!out.empty() && out.back() == '/') {
			out.pop_back();
		}
		if (out.empty()) {
			out = "unnamed";
		}
		return out;
	}

	// Resolves `rel` against `dir` and accepts it only when the canonical result stays inside the
	// canonical `dir` (compared by path components, symlinks resolved). Absolute `rel` is accepted
	// only with `allow_absolute`, and then it still has to be inside `dir`.
	inline std::optional<std::filesystem::path> path_within_dir(
		const std::filesystem::path& dir, std::string_view rel_text, const bool allow_absolute = false)
	{
		namespace fs = std::filesystem;

		std::string text{ rel_text };
		for (char& c : text) {
			if (c == '\\') {
				c = '/';
			}
		}
		if (text.empty() || has_control_chars(text)) {
			return std::nullopt;
		}

		const fs::path rel{ text };
		const bool absolute = rel.is_absolute();
		if (absolute && !allow_absolute) {
			return std::nullopt;
		}
		if (!absolute) {
			if (rel.has_root_name() || rel.has_root_directory()) {
				return std::nullopt;
			}
			for (const auto& part : rel) {
				const std::string name = part.generic_string();
				if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
					return std::nullopt;
				}
			}
		}

		std::error_code ec;
		fs::path canon_dir = fs::weakly_canonical(dir, ec);
		if (ec) {
			return std::nullopt;
		}
		fs::path candidate = fs::weakly_canonical(absolute ? rel : canon_dir / rel, ec);
		if (ec) {
			return std::nullopt;
		}

		if (!canon_dir.has_filename()) {
			canon_dir = canon_dir.parent_path();
		}
		auto cand_it = candidate.begin();
		for (auto dir_it = canon_dir.begin(); dir_it != canon_dir.end(); ++dir_it, ++cand_it) {
			if (cand_it == candidate.end() || *dir_it != *cand_it) {
				return std::nullopt;
			}
		}
		return candidate;
	}

	// Reads a regular file of at most `max_bytes`. Refuses directories, FIFOs and devices (which
	// would block or report no size) instead of throwing or allocating a bogus buffer.
	inline std::optional<std::vector<std::byte>> read_regular_file_capped(
		const std::filesystem::path& path, const std::uintmax_t max_bytes = max_external_file_bytes)
	{
		namespace fs = std::filesystem;

		std::error_code ec;
		if (!fs::is_regular_file(path, ec) || ec) {
			return std::nullopt;
		}
		const auto file_bytes = fs::file_size(path, ec);
		if (ec || file_bytes > max_bytes) {
			return std::nullopt;
		}

		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in) {
			return std::nullopt;
		}
		const auto end = in.tellg();
		if (end < 0 || static_cast<std::uintmax_t>(end) > max_bytes) {
			return std::nullopt;
		}
		in.seekg(0);

		std::vector<std::byte> bytes(static_cast<std::size_t>(end));
		if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			return std::nullopt;
		}
		return bytes;
	}

	// Model-relative file read: nullopt when `rel` leaves `dir`, is not a regular file, or is too big.
	inline std::optional<std::vector<std::byte>> read_file_within_dir(
		const std::filesystem::path& dir, const std::string_view rel,
		const std::uintmax_t max_bytes = max_external_file_bytes)
	{
		const auto path = path_within_dir(dir, rel);
		if (!path.has_value()) {
			return std::nullopt;
		}
		return read_regular_file_capped(*path, max_bytes);
	}
}
