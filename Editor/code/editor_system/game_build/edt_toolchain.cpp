#include "edt_toolchain.h"
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

#ifndef _WIN32
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	fs::path path_of(std::string_view utf8)
	{
		return fs::path(std::u8string(utf8.begin(), utf8.end()));
	}

	std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
		return text;
	}

	char upper(char c)
	{
		return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
	}

	bool same_name(std::string_view a, std::string_view b, edt::env_flavor flavor)
	{
		if (a.size() != b.size()) return false;
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (flavor == edt::env_flavor::windows ? upper(a[i]) != upper(b[i]) : a[i] != b[i]) return false;
		}
		return true;
	}

	// The entry of `name`, or null.
	std::string* entry_of(std::vector<std::string>& environment, std::string_view name, edt::env_flavor flavor)
	{
		for (auto& entry : environment) {
			const std::size_t equals = entry.find('=', 1);
			if (equals != std::string::npos && same_name(std::string_view(entry).substr(0, equals), name, flavor)) return &entry;
		}
		return nullptr;
	}

	void append_utf8(std::string& out, char32_t c)
	{
		if (c < 0x80) {
			out += static_cast<char>(c);
		} else if (c < 0x800) {
			out += static_cast<char>(0xC0 | (c >> 6));
			out += static_cast<char>(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out += static_cast<char>(0xE0 | (c >> 12));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (c & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (c >> 18));
			out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (c & 0x3F));
		}
	}

	bool is_file(const fs::path& path)
	{
		std::error_code ec;
		return fs::is_regular_file(path, ec);
	}

	// The cmake of build/cmake: it must exist.
	std::optional<std::string> check_cmake_override(const fs::path& cmake)
	{
		if (cmake.empty() || is_file(cmake)) return std::nullopt;
		return std::format("CMake was not found at '{}' (build/cmake)", utf8_of(cmake));
	}

#ifdef _WIN32
	std::string own_variable(const char* name)
	{
		char* value = nullptr;
		std::size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
		std::string out{ value };
		std::free(value);
		return out;
	}

	std::variant<edt::toolchain, std::string> find_windows(const edt::toolchain_settings& settings,
		const edt::process_runner& run, std::stop_token stop)
	{
		const std::string min_version = settings.vs_min_version.empty() ? std::string{ "17.14" } : settings.vs_min_version;
		const auto no_vs = [&min_version](const std::string& why) {
			return std::format("No Visual Studio 2022 {} or later with 'Desktop development with C++' was found (vswhere: {})",
				min_version, why);
		};

		fs::path vs = settings.vs_path;
		if (vs.empty()) {
			std::string program_files = own_variable("ProgramFiles(x86)");
			if (program_files.empty()) program_files = "C:\\Program Files (x86)";
			const fs::path vswhere = path_of(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
			if (!is_file(vswhere)) return no_vs(std::format("not found at '{}'", utf8_of(vswhere)));
			std::vector<std::string> lines;
			const auto result = run(vswhere, { "-latest", "-products", "*", "-requires",
				"Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-version", std::format("[{},)", min_version),
				"-property", "installationPath", "-nologo", "-utf8" }, {},
				[&lines](std::string_view line) { lines.emplace_back(line); }, stop);
			if (!result.error.empty()) return no_vs(result.error);
			if (result.cancelled) return std::string{ "cancelled" };
			const auto found = edt::parse_vswhere_output(lines);
			if (!found) return no_vs(result.exit_code == 0 ? std::string{ "no matching installation" }
				: std::format("exit code {}", result.exit_code));
			vs = *found;
		}

		const fs::path vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
		if (!is_file(vcvars)) return no_vs(std::format("'{}' has no VC\\Auxiliary\\Build\\vcvars64.bat", utf8_of(vs)));

		if (auto unsafe = edt::cmd_unsafe_path_error(vcvars)) return std::move(*unsafe);

		std::string comspec = own_variable("ComSpec");
		if (comspec.empty()) comspec = own_variable("SystemRoot") + "\\System32\\cmd.exe";
		// The command after /c starts with an unquoted word, so cmd strips no quote (/s rule): the quotes that
		// quote_windows_arg puts around the path of vcvars64.bat (it has spaces) reach `call` as they are.
		// /u: `set` writes UTF-16LE, not the OEM code page, so non-ASCII values (C:\Users\<name>) survive. Everything
		// vcvars64.bat prints goes to nul, or its 8-bit text would break the UTF-16 stream.
		platform::process_options raw;
		raw.raw_output = true;
		std::string output;
		const auto result = run(path_of(comspec), { "/u", "/d", "/v:off", "/s", "/c", "call", utf8_of(vcvars), ">nul", "2>&1",
			"&&", "set" }, raw, [&output](std::string_view chunk) { output.append(chunk); }, stop);
		if (result.cancelled) return std::string{ "cancelled" };
		if (!result.error.empty() || result.exit_code != 0) {
			return std::format("The Visual Studio environment could not be set up ('{}' failed: {})", utf8_of(vcvars),
				result.error.empty() ? std::format("exit code {}", result.exit_code) : result.error);
		}
		std::vector<std::string> environment = edt::parse_set_output_utf16le(output);
		if (!edt::variable_of(environment, "PATH", edt::env_flavor::windows)) {
			return std::format("The Visual Studio environment could not be set up ('{}' printed no PATH)", utf8_of(vcvars));
		}
		// English MSVC messages for build_output_parser.
		edt::set_variable(environment, "VSLANG", "1033", edt::env_flavor::windows);

		const fs::path vs_cmake_dir = vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake";
		if (!edt::find_on_path(environment, "ninja.exe", edt::env_flavor::windows) && is_file(vs_cmake_dir / "Ninja" / "ninja.exe")) {
			edt::merge_path(environment, vs_cmake_dir / "Ninja", edt::env_flavor::windows);
		}
		if (!edt::find_on_path(environment, "ninja.exe", edt::env_flavor::windows)) {
			return std::string{ "Ninja was not found: install the 'C++ CMake tools for Windows' component of Visual Studio "
				"or put ninja.exe on PATH" };
		}

		if (auto error = check_cmake_override(settings.cmake)) return std::move(*error);
		fs::path cmake = settings.cmake;
		if (cmake.empty() && is_file(vs_cmake_dir / "CMake" / "bin" / "cmake.exe")) cmake = vs_cmake_dir / "CMake" / "bin" / "cmake.exe";
		if (cmake.empty()) {
			if (auto found = edt::find_on_path(environment, "cmake.exe", edt::env_flavor::windows)) cmake = std::move(*found);
		}
		if (cmake.empty()) return std::string{ "CMake was not found: install it or set build/cmake" };

		return edt::toolchain{ cmake, std::move(environment),
			std::format("Visual Studio at '{}', CMake '{}'", utf8_of(vs), utf8_of(cmake)) };
	}
#else
	std::vector<std::string> own_environment()
	{
		std::vector<std::string> out;
		for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) out.emplace_back(*entry);
		return out;
	}

	std::variant<edt::toolchain, std::string> find_posix(const edt::toolchain_settings& settings)
	{
		std::vector<std::string> environment = own_environment();
		// English GCC/Clang messages (and plain quotes) for build_output_parser.
		edt::set_variable(environment, "LC_ALL", "C", edt::env_flavor::posix);

		if (auto error = check_cmake_override(settings.cmake)) return std::move(*error);
		fs::path cmake = settings.cmake;
		if (cmake.empty()) {
			if (auto found = edt::find_on_path(environment, "cmake", edt::env_flavor::posix)) cmake = std::move(*found);
		}
		if (cmake.empty()) return std::string{ "CMake was not found: install it or set build/cmake" };
		if (!edt::find_on_path(environment, "ninja", edt::env_flavor::posix)) {
			return std::string{ "Ninja was not found: install it (the ninja-build package) or put it on PATH" };
		}
		return edt::toolchain{ cmake, std::move(environment), std::format("CMake '{}'", utf8_of(cmake)) };
	}
#endif
}

std::optional<fs::path> edt::parse_vswhere_output(const std::vector<std::string>& lines)
{
	for (const auto& raw : lines) {
		const std::string_view line = trim(raw);
		const bool drive = line.size() >= 3 && ((line[0] >= 'A' && line[0] <= 'Z') || (line[0] >= 'a' && line[0] <= 'z'))
			&& line[1] == ':' && (line[2] == '\\' || line[2] == '/');
		const bool unc = line.size() >= 3 && line[0] == '\\' && line[1] == '\\';
		if (drive || unc) return path_of(line);
	}
	return std::nullopt;
}

std::vector<std::string> edt::parse_set_output(const std::vector<std::string>& lines)
{
	std::vector<std::string> out;
	for (const auto& raw : lines) {
		std::string_view line = raw;
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
		const std::size_t equals = line.find('=');
		// 0: cmd's hidden per-drive directories ("=C:=C:\dir"); npos: banners and blank lines.
		if (equals == 0 || equals == std::string_view::npos) continue;
		bool name_ok = true;
		for (const char c : line.substr(0, equals)) {
			if (c == ' ' || c == '\t' || static_cast<unsigned char>(c) < 0x20 || c == '[' || c == '*') name_ok = false;
		}
		if (name_ok) out.emplace_back(line);
	}
	return out;
}

std::string edt::utf16le_to_utf8(std::string_view bytes)
{
	const auto unit = [bytes](std::size_t index) {
		return static_cast<char32_t>(static_cast<unsigned char>(bytes[2 * index])
			| (static_cast<unsigned>(static_cast<unsigned char>(bytes[2 * index + 1])) << 8));
	};
	const std::size_t count = bytes.size() / 2;
	std::string out;
	out.reserve(bytes.size());
	std::size_t i = count > 0 && unit(0) == 0xFEFF ? 1 : 0;
	for (; i < count; ++i) {
		char32_t c = unit(i);
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < count && unit(i + 1) >= 0xDC00 && unit(i + 1) <= 0xDFFF) {
			c = static_cast<char32_t>(0x10000 + ((c - 0xD800) << 10) + (unit(i + 1) - 0xDC00));
			++i;
		} else if (c >= 0xD800 && c <= 0xDFFF) {
			c = 0xFFFD;
		}
		append_utf8(out, c);
	}
	return out;
}

std::vector<std::string> edt::parse_set_output_utf16le(std::string_view bytes)
{
	const std::string text = utf16le_to_utf8(bytes);
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < text.size()) {
		const std::size_t end = std::min(text.find('\n', start), text.size());
		lines.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return parse_set_output(lines);
}

std::optional<std::string> edt::cmd_unsafe_path_error(const fs::path& path)
{
	const std::string text = utf8_of(path);
	if (text.find_first_of("&|<>^%!") == std::string::npos) return std::nullopt;
	return std::format("The Visual Studio path '{}' contains a character that cmd.exe interprets (& | < > ^ % !): "
		"use an installation in a folder without them (build/vs_path)", text);
}

std::optional<std::string> edt::variable_of(const std::vector<std::string>& environment, std::string_view name, env_flavor flavor)
{
	for (const auto& entry : environment) {
		const std::size_t equals = entry.find('=', 1);
		if (equals != std::string::npos && same_name(std::string_view(entry).substr(0, equals), name, flavor)) {
			return entry.substr(equals + 1);
		}
	}
	return std::nullopt;
}

void edt::set_variable(std::vector<std::string>& environment, std::string_view name, std::string_view value, env_flavor flavor)
{
	if (std::string* entry = entry_of(environment, name, flavor)) {
		entry->resize(entry->find('=', 1) + 1);
		entry->append(value);
		return;
	}
	environment.push_back(std::format("{}={}", name, value));
}

void edt::merge_path(std::vector<std::string>& environment, const fs::path& dir, env_flavor flavor)
{
	const char separator = flavor == env_flavor::windows ? ';' : ':';
	const std::string text = utf8_of(dir);
	if (std::string* entry = entry_of(environment, "PATH", flavor)) {
		const std::size_t equals = entry->find('=', 1);
		const std::string rest = entry->substr(equals + 1);
		entry->resize(equals + 1);
		entry->append(text);
		if (!rest.empty()) {
			entry->push_back(separator);
			entry->append(rest);
		}
		return;
	}
	environment.push_back("PATH=" + text);
}

std::optional<fs::path> edt::find_on_path(const std::vector<std::string>& environment, std::string_view name, env_flavor flavor)
{
	const std::string value = variable_of(environment, "PATH", flavor).value_or(std::string{});
	const char separator = flavor == env_flavor::windows ? ';' : ':';
	std::size_t begin = 0;
	while (begin <= value.size()) {
		const std::size_t end = std::min(value.find(separator, begin), value.size());
		std::string_view dir = std::string_view(value).substr(begin, end - begin);
		if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
		if (!dir.empty()) {
			fs::path candidate = path_of(dir) / path_of(name);
			if (is_file(candidate)) return candidate;
		}
		begin = end + 1;
	}
	return std::nullopt;
}

std::variant<edt::toolchain, std::string> edt::find_toolchain(const toolchain_settings& settings, const process_runner& run,
	std::stop_token stop)
{
#ifdef _WIN32
	return find_windows(settings, run, stop);
#else
	(void)run;
	(void)stop;
	return find_posix(settings);
#endif
}

std::variant<edt::toolchain, std::string> edt::toolchain_cache::get(const toolchain_settings& settings,
	const process_runner& run, std::stop_token stop)
{
	const std::lock_guard lock(m_mutex);
	if (m_found && m_settings && m_settings->cmake == settings.cmake && m_settings->vs_path == settings.vs_path
		&& m_settings->vs_min_version == settings.vs_min_version) {
		return *m_found;
	}
	auto found = find_toolchain(settings, run, stop);
	if (const toolchain* ok = std::get_if<toolchain>(&found)) {
		m_settings = settings;
		m_found = *ok;
		egLOG_INFO("editor/build", "toolchain: {}", ok->description);
	}
	return found;
}
