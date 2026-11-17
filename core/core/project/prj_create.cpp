#include <snk/project/prj_create.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	// Not path::string(): on Windows it goes through the ANSI code page and throws for what that cannot hold.
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	// `inner` is `outer` or lies under it; both already absolute and normalized.
	bool is_within(const fs::path& inner, const fs::path& outer)
	{
		const fs::path relative = inner.lexically_relative(outer);
		return !relative.empty() && *relative.begin() != "..";
	}

	bool is_valid_name(std::string_view name)
	{
		if (name.empty() || name.size() > 128) {
			return false;
		}
		for (const char c : name) {
			const auto u = static_cast<unsigned char>(c);
			if (u < 0x20 || u == 0x7f) {
				return false;
			}
		}
		return true;
	}

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	// What a new project's repository must never carry (#912): the person's MCP token, caches, build trees. A line is
	// there when the file has it or one of the lines that ignore at least as much.
	struct ignored_line
	{
		std::string_view line;
		std::array<std::string_view, 3> covered_by;
	};
	constexpr std::array<ignored_line, 3> k_ignored_lines{ {
		{ ".snk/mcp.json", { ".snk/*", ".snk/", ".snk" } },
		{ ".cache/", { ".cache", "/.cache/", "/.cache" } },
		{ "build/", { "build", "/build/", "/build" } } } };

	// The project's .gitignore with k_ignored_lines: written, or the lines the template's file lacks added at its end.
	std::optional<std::string> ignore_private_files(const fs::path& project_dir)
	{
		const fs::path file = project_dir / ".gitignore";
		std::string text = read_text(file);
		// A file written with CRLF keeps them: git takes a mixed file as changed whenever autocrlf touches it.
		const std::string_view eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
		std::string added;
		for (const auto& [line, covered_by] : k_ignored_lines) {
			bool present = false;
			std::size_t start = 0;
			while (start <= text.size() && !present) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();
				std::string_view existing(text.data() + start, end - start);
				if (!existing.empty() && existing.back() == '\r') existing.remove_suffix(1);
				present = existing == line || std::ranges::find(covered_by, existing) != covered_by.end();
				start = end + 1;
			}
			if (!present) {
				added += line;
				added += eol;
			}
		}
		if (added.empty()) return std::nullopt;
		if (!text.empty() && text.back() != '\n') text += eol;
		text += added;
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << text;
		out.close();
		if (out.fail()) return std::format("cannot write '{}'", utf8_of(file));
		return std::nullopt;
	}

	// Writes the copied manifest with the new project's name and this engine's version; the rest stays as the
	// template has it.
	std::optional<std::string> stamp_manifest(const fs::path& manifest_file, std::string_view name)
	{
		std::error_code ec;
		json::value root = json::parse(read_text(manifest_file), ec);
		if (ec || !root.is_object()) {
			return std::format("the template manifest '{}' is not a JSON object", utf8_of(manifest_file));
		}
		root.as_object()["name"] = name;
		root.as_object()["engine_version"] = prj::to_string(prj::current_engine_version());

		const std::string text = prj::to_indented_json(root);
		std::ofstream out(manifest_file, std::ios::binary | std::ios::trunc);
		out << text;
		out.close();
		if (out.fail()) {
			return std::format("cannot write '{}'", utf8_of(manifest_file));
		}
		return std::nullopt;
	}
}

prj::create_result prj::create_project(const fs::path& template_dir, const fs::path& requested_dir, std::string_view name,
	const prepare_project& prepare)
{
	create_result result;
	std::error_code ec;
	fs::path target_dir = fs::absolute(requested_dir, ec).lexically_normal();
	if (!target_dir.has_filename()) {
		target_dir = target_dir.parent_path(); // "games/new/"
	}
	if (ec || !target_dir.has_filename()) {
		result.error = std::format("'{}' cannot be the folder of a project", utf8_of(requested_dir));
		return result;
	}
	if (!is_valid_name(name)) {
		result.error = "the project name must be 1 to 128 characters without control characters";
		return result;
	}
	if (const auto checked = load_manifest(template_dir / k_manifest_file_name); !checked.data) {
		result.error = std::format("the project template '{}' is not usable: {}", utf8_of(template_dir),
			checked.error ? to_string(*checked.error) : std::string{ "unknown error" });
		return result;
	}

	// The staging folder would be made inside the tree being copied.
	{
		std::error_code canonical_ec;
		const fs::path template_real = fs::weakly_canonical(fs::absolute(template_dir, canonical_ec), canonical_ec);
		const fs::path target_real = fs::weakly_canonical(target_dir, canonical_ec);
		if (canonical_ec || is_within(target_real, template_real)) {
			result.error = std::format("'{}' cannot be made inside the project template '{}'", utf8_of(target_dir),
				utf8_of(template_dir));
			return result;
		}
	}

	const auto status = fs::symlink_status(target_dir, ec);
	if (fs::exists(status)) {
		if (!fs::is_directory(status) || !fs::is_empty(target_dir, ec) || ec) {
			result.error = std::format("'{}' already exists and is not an empty folder", utf8_of(target_dir));
			return result;
		}
	}

	// Built next to the target and renamed into place, so a failure halfway never leaves a half-made project there.
	std::random_device random;
	fs::path staging_name{ "." };
	staging_name += target_dir.filename();
	staging_name += std::format(".{:08x}.new", random());
	const fs::path staging = target_dir.parent_path() / staging_name;
	const auto fail = [&](std::string message) {
		std::error_code ignored;
		fs::remove_all(staging, ignored);
		result.error = std::move(message);
		return result;
	};

	fs::create_directories(target_dir.parent_path(), ec);
	if (ec) {
		return fail(std::format("cannot create '{}': {}", utf8_of(target_dir.parent_path()), ec.message()));
	}
	fs::copy(template_dir, staging, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
	if (ec) {
		return fail(std::format("cannot copy the template to '{}': {}", utf8_of(target_dir), ec.message()));
	}
	if (auto problem = stamp_manifest(staging / k_manifest_file_name, name)) {
		return fail(std::move(*problem));
	}
	if (auto problem = ignore_private_files(staging)) {
		return fail(std::move(*problem));
	}
	if (prepare) {
		std::string problem;
		try {
			problem = prepare(staging);
		} catch (const std::exception& error) {
			problem = std::format("cannot prepare the new project: {}", error.what());
		}
		if (!problem.empty()) {
			return fail(std::move(problem));
		}
	}
	if (fs::exists(status)) {
		fs::remove(target_dir, ec); // the empty folder the user picked
	}
	fs::rename(staging, target_dir, ec);
	if (ec) {
		return fail(std::format("cannot move the new project to '{}': {}", utf8_of(target_dir), ec.message()));
	}
	result.manifest_file = target_dir / k_manifest_file_name;
	return result;
}
