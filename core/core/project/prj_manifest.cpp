#include <snk/project/prj_manifest.h>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <system_error>
#include <utility>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <snk/common/logger/engine_log.h>

namespace json = boost::json;

namespace
{
	constexpr std::uintmax_t k_max_manifest_bytes = 1024 * 1024;

	// Thrown inside parse_manifest only, turned into manifest_result::error before it returns.
	struct manifest_failure
	{
		std::string json_path;
		std::string reason;
	};

	[[noreturn]] void fail(std::string json_path, std::string reason)
	{
		throw manifest_failure{ std::move(json_path), std::move(reason) };
	}

	bool has_control_chars(std::string_view s)
	{
		for (const char c : s) {
			const auto u = static_cast<unsigned char>(c);
			if (u < 0x20 || u == 0x7f) {
				return true;
			}
		}
		return false;
	}

	// CON, PRN, AUX, NUL, COM1-9, LPT1-9: Windows opens a device for them, with any extension and in any case.
	bool is_windows_device_name(std::string_view part)
	{
		const std::string_view stem = part.substr(0, part.find('.'));
		std::string upper;
		for (const char c : stem) {
			upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
			return true;
		}
		return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' && upper[3] <= '9';
	}

	// A content root must stay inside the project and mean the same directory on every platform: only '/' separates,
	// no absolute paths, drives or ':' (which would also spell a protocol), no backslashes, no "." or ".." or empty
	// parts, no Windows device names. A trailing '/' is allowed.
	bool is_safe_relative_dir(std::string_view text)
	{
		if (text.empty() || has_control_chars(text) || text.find_first_of(":\\") != std::string_view::npos) {
			return false;
		}
		if (text.front() == '/') {
			return false;
		}
		std::size_t pos = 0;
		while (pos <= text.size()) {
			std::size_t end = text.find('/', pos);
			if (end == std::string_view::npos) {
				end = text.size();
			}
			const std::string_view part = text.substr(pos, end - pos);
			const bool trailing_separator = part.empty() && end == text.size() && pos != 0;
			if ((part.empty() && !trailing_separator) || part == "." || part == ".." || is_windows_device_name(part)) {
				return false;
			}
			pos = end + 1;
		}
		return true;
	}

	// "res/" and "res" name the same directory.
	std::string normalized_dir(std::string_view text)
	{
		std::string out{ text };
		while (!out.empty() && out.back() == '/') {
			out.pop_back();
		}
		return out;
	}

	// Unknown keys come from the file: control characters would forge log lines, so they are shown as \u00XX.
	std::string printable(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (const char c : text) {
			const auto u = static_cast<unsigned char>(c);
			if (u < 0x20 || u == 0x7f) {
				out += std::format("\\u{:04x}", static_cast<unsigned>(u));
			} else {
				out += c;
			}
		}
		return out;
	}

	// A module name becomes a file name (game.dll): [A-Za-z][A-Za-z0-9_-]{0,63}.
	bool is_module_name(std::string_view name)
	{
		if (name.empty() || name.size() > 64 || !std::isalpha(static_cast<unsigned char>(name.front()))) {
			return false;
		}
		for (const char c : name) {
			const auto u = static_cast<unsigned char>(c);
			if (u >= 0x80 || !(std::isalnum(u) || c == '_' || c == '-')) {
				return false;
			}
		}
		return true;
	}

	std::string lowercase(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (const char c : text) {
			out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}

	bool is_res_tag(std::string_view text)
	{
		constexpr std::string_view prefix = "res://";
		if (!text.starts_with(prefix) || has_control_chars(text)) {
			return false;
		}
		const std::string_view rel = text.substr(prefix.size());
		return is_safe_relative_dir(rel) && rel.back() != '/';
	}

	// The view points into `value`, which outlives the parse.
	std::string_view require_string(const json::value& value, const std::string& path)
	{
		if (!value.is_string()) {
			fail(path, "expected a string");
		}
		const auto& text = value.get_string();
		return { text.data(), text.size() };
	}

	std::string require_non_empty_string(const json::value& value, const std::string& path)
	{
		const std::string_view text = require_string(value, path);
		if (text.empty() || has_control_chars(text)) {
			fail(path, "expected a non-empty string without control characters");
		}
		return std::string{ text };
	}

	// Checks only: binding the result of a reference-returning helper that takes a temporary path string trips
	// GCC's -Wdangling-reference, so callers take value.get_array() themselves.
	void require_array(const json::value& value, const std::string& path)
	{
		if (!value.is_array()) {
			fail(path, "expected an array");
		}
	}

	int require_int(const json::value& value, const std::string& path)
	{
		std::int64_t number = 0;
		if (value.is_int64()) {
			number = value.get_int64();
		} else if (value.is_uint64()) {
			fail(path, "integer out of range");
		} else {
			fail(path, "expected an integer");
		}
		if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
			fail(path, "integer out of range");
		}
		return static_cast<int>(number);
	}

	void warn_unknown_fields(const json::object& object, std::initializer_list<std::string_view> known,
		const std::string& path, std::vector<std::string>& warnings)
	{
		for (const auto& entry : object) {
			const std::string_view key{ entry.key().data(), entry.key().size() };
			bool is_known = false;
			for (const auto name : known) {
				is_known = is_known || key == name;
			}
			if (!is_known) {
				warnings.push_back(std::format("{}.{}: unknown field, ignored", path, printable(key)));
			}
		}
	}

	prj::module_ref parse_module(const json::value& value, const std::string& path, std::vector<std::string>& warnings)
	{
		if (!value.is_object()) {
			fail(path, "expected an object");
		}
		const auto& object = value.get_object();
		warn_unknown_fields(object, { "name", "kind", "optional" }, path, warnings);

		prj::module_ref ref;
		const auto* name = object.if_contains("name");
		if (!name) {
			fail(path + ".name", "required field is missing");
		}
		ref.name = require_non_empty_string(*name, path + ".name");
		if (!is_module_name(ref.name)) {
			fail(path + ".name", "expected a letter, then up to 63 letters, digits, '_' or '-'");
		}

		if (const auto* kind = object.if_contains("kind")) {
			const std::string_view text = require_string(*kind, path + ".kind");
			if (text == "runtime") {
				ref.kind = prj::module_kind::runtime;
			} else if (text == "editor") {
				ref.kind = prj::module_kind::editor;
			} else {
				fail(path + ".kind", "expected \"runtime\" or \"editor\"");
			}
		}
		if (const auto* optional = object.if_contains("optional")) {
			if (!optional->is_bool()) {
				fail(path + ".optional", "expected a boolean");
			}
			ref.optional = optional->get_bool();
		}
		return ref;
	}

	prj::manifest parse_object(const json::object& root, std::vector<std::string>& warnings)
	{
		warn_unknown_fields(root,
			{ "format_version", "name", "engine_version", "start_scene", "content_roots", "modules", "platforms" },
			"$", warnings);

		const auto required = [&root](std::string_view key) -> const json::value& {
			const auto* value = root.if_contains(json::string_view{ key.data(), key.size() });
			if (!value) {
				fail(std::format("$.{}", key), "required field is missing");
			}
			return *value;
		};

		prj::manifest result;

		// Checked first: a newer format may have changed the meaning of every other field.
		result.format_version = require_int(required("format_version"), "$.format_version");
		if (result.format_version < 1) {
			fail("$.format_version", "must be 1 or greater");
		}
		if (result.format_version > prj::k_manifest_format_version) {
			fail("$.format_version", std::format("format {} is newer than the supported format {}; update the editor",
				result.format_version, prj::k_manifest_format_version));
		}

		result.name = require_non_empty_string(required("name"), "$.name");

		const std::string_view engine_text = require_string(required("engine_version"), "$.engine_version");
		const auto engine = prj::parse_engine_version(engine_text);
		if (!engine) {
			fail("$.engine_version", "expected \"MAJOR.MINOR.PATCH\"");
		}
		result.engine = *engine;

		if (const auto* start_scene = root.if_contains("start_scene")) {
			const std::string_view text = require_string(*start_scene, "$.start_scene");
			if (!is_res_tag(text)) {
				fail("$.start_scene", "expected a res:// tag inside the project");
			}
			result.start_scene = std::string{ text };
		}

		if (const auto* roots = root.if_contains("content_roots")) {
			require_array(*roots, "$.content_roots");
			const auto& array = roots->get_array();
			if (array.empty()) {
				fail("$.content_roots", "expected at least one content root");
			}
			std::set<std::string> seen;
			for (std::size_t i = 0; i < array.size(); ++i) {
				const std::string path = std::format("$.content_roots[{}]", i);
				const std::string_view text = require_string(array[i], path);
				if (!is_safe_relative_dir(text)) {
					fail(path, "expected a relative directory inside the project");
				}
				if (!seen.insert(normalized_dir(text)).second) {
					fail(path, "duplicate content root");
				}
				result.content_roots.emplace_back(std::string{ text });
			}
		} else {
			result.content_roots.emplace_back("res");
		}

		if (const auto* modules = root.if_contains("modules")) {
			require_array(*modules, "$.modules");
			const auto& array = modules->get_array();
			std::set<std::string> seen;
			for (std::size_t i = 0; i < array.size(); ++i) {
				const std::string path = std::format("$.modules[{}]", i);
				auto ref = parse_module(array[i], path, warnings);
				// "Game" and "game" are the same file on Windows and macOS.
				if (!seen.insert(lowercase(ref.name)).second) {
					fail(path + ".name", "duplicate module name");
				}
				result.modules.push_back(std::move(ref));
			}
		}

		if (const auto* platforms = root.if_contains("platforms")) {
			require_array(*platforms, "$.platforms");
			const auto& array = platforms->get_array();
			for (std::size_t i = 0; i < array.size(); ++i) {
				result.platforms.push_back(require_non_empty_string(array[i], std::format("$.platforms[{}]", i)));
			}
		}

		return result;
	}

	std::optional<std::string> read_capped(const std::filesystem::path& file, std::string& reason)
	{
		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec)) {
			reason = std::format("project manifest not found: {}", file.string());
			return std::nullopt;
		}
		const auto size = std::filesystem::file_size(file, ec);
		if (ec || size > k_max_manifest_bytes) {
			reason = std::format("project manifest is unreadable or larger than {} bytes: {}", k_max_manifest_bytes, file.string());
			return std::nullopt;
		}
		std::ifstream in(file, std::ios::binary);
		std::string text(static_cast<std::size_t>(size), '\0');
		if (!in || (size > 0 && !in.read(text.data(), static_cast<std::streamsize>(size)))) {
			reason = std::format("failed to read project manifest: {}", file.string());
			return std::nullopt;
		}
		return text;
	}
}

prj::manifest_result prj::parse_manifest(std::string_view text)
{
	manifest_result result;

	json::error_code ec;
	const json::value root = json::parse(json::string_view{ text.data(), text.size() }, ec);
	if (ec) {
		result.error = manifest_error{ "", std::format("invalid JSON: {}", ec.message()) };
		return result;
	}
	if (!root.is_object()) {
		result.error = manifest_error{ "$", "expected a JSON object" };
		return result;
	}

	try {
		result.data = parse_object(root.get_object(), result.warnings);
	} catch (const manifest_failure& failure) {
		result.error = manifest_error{ failure.json_path, failure.reason };
	}
	return result;
}

prj::manifest_result prj::load_manifest(const std::filesystem::path& path)
{
	std::error_code ec;
	const auto file = std::filesystem::is_directory(path, ec) ? path / k_manifest_file_name : path;

	std::string reason;
	const auto text = read_capped(file, reason);
	if (!text) {
		manifest_result result;
		result.error = manifest_error{ "", std::move(reason) };
		return result;
	}

	auto result = parse_manifest(*text);
	for (const auto& warning : result.warnings) {
		egLOG_WARN("project", "{}: {}", file.string(), warning);
	}
	return result;
}

std::string prj::to_string(const manifest_error& error)
{
	if (error.json_path.empty()) {
		return error.reason;
	}
	return std::format("{}: {}", error.json_path, error.reason);
}
