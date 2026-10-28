#include <snk/project/prj_create.h>
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	// Boost.JSON writes one line; a manifest is read and edited by people.
	void write_indented(std::string& out, const json::value& value, int depth)
	{
		const auto newline = [&out](int level) {
			out += '\n';
			out.append(static_cast<std::size_t>(level) * 4, ' ');
		};
		if (value.is_object() && !value.as_object().empty()) {
			out += '{';
			bool first = true;
			for (const auto& [key, item] : value.as_object()) {
				out += first ? "" : ",";
				first = false;
				newline(depth + 1);
				out += json::serialize(json::value{ key });
				out += ": ";
				write_indented(out, item, depth + 1);
			}
			newline(depth);
			out += '}';
		} else if (value.is_array() && !value.as_array().empty()) {
			out += '[';
			bool first = true;
			for (const auto& item : value.as_array()) {
				out += first ? "" : ",";
				first = false;
				newline(depth + 1);
				write_indented(out, item, depth + 1);
			}
			newline(depth);
			out += ']';
		} else {
			out += json::serialize(value);
		}
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

	// Writes the copied manifest with the new project's name and this engine's version; the rest stays as the
	// template has it.
	std::optional<std::string> stamp_manifest(const fs::path& manifest_file, std::string_view name)
	{
		std::error_code ec;
		json::value root = json::parse(read_text(manifest_file), ec);
		if (ec || !root.is_object()) {
			return std::format("the template manifest '{}' is not a JSON object", manifest_file.string());
		}
		root.as_object()["name"] = name;
		root.as_object()["engine_version"] = prj::to_string(prj::current_engine_version());

		std::string text;
		write_indented(text, root, 0);
		text += '\n';
		std::ofstream out(manifest_file, std::ios::binary | std::ios::trunc);
		out << text;
		out.close();
		if (out.fail()) {
			return std::format("cannot write '{}'", manifest_file.string());
		}
		return std::nullopt;
	}
}

prj::create_result prj::create_project(const fs::path& template_dir, const fs::path& requested_dir, std::string_view name)
{
	create_result result;
	std::error_code ec;
	fs::path target_dir = fs::absolute(requested_dir, ec).lexically_normal();
	if (!target_dir.has_filename()) {
		target_dir = target_dir.parent_path(); // "games/new/"
	}
	if (ec || !target_dir.has_filename()) {
		result.error = std::format("'{}' cannot be the folder of a project", requested_dir.string());
		return result;
	}
	if (!is_valid_name(name)) {
		result.error = "the project name must be 1 to 128 characters without control characters";
		return result;
	}
	if (const auto checked = load_manifest(template_dir / k_manifest_file_name); !checked.data) {
		result.error = std::format("the project template '{}' is not usable: {}", template_dir.string(),
			checked.error ? to_string(*checked.error) : std::string{ "unknown error" });
		return result;
	}

	const auto status = fs::symlink_status(target_dir, ec);
	if (fs::exists(status)) {
		if (!fs::is_directory(status) || !fs::is_empty(target_dir, ec) || ec) {
			result.error = std::format("'{}' already exists and is not an empty folder", target_dir.string());
			return result;
		}
	}

	// Built next to the target and renamed into place, so a failure halfway never leaves a half-made project there.
	std::random_device random;
	const fs::path staging = target_dir.parent_path() / std::format(".{}.{:08x}.new", target_dir.filename().string(), random());
	const auto fail = [&](std::string message) {
		std::error_code ignored;
		fs::remove_all(staging, ignored);
		result.error = std::move(message);
		return result;
	};

	fs::create_directories(target_dir.parent_path(), ec);
	if (ec) {
		return fail(std::format("cannot create '{}': {}", target_dir.parent_path().string(), ec.message()));
	}
	fs::copy(template_dir, staging, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
	if (ec) {
		return fail(std::format("cannot copy the template to '{}': {}", target_dir.string(), ec.message()));
	}
	if (auto problem = stamp_manifest(staging / k_manifest_file_name, name)) {
		return fail(std::move(*problem));
	}
	if (fs::exists(status)) {
		fs::remove(target_dir, ec); // the empty folder the user picked
	}
	fs::rename(staging, target_dir, ec);
	if (ec) {
		return fail(std::format("cannot move the new project to '{}': {}", target_dir.string(), ec.message()));
	}
	result.manifest_file = target_dir / k_manifest_file_name;
	return result;
}
