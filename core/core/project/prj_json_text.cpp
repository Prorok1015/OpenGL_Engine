#include <snk/project/prj_json_text.h>
#include <cstddef>
#include <format>
#include <fstream>
#include <random>
#include <span>
#include <system_error>
#include <vector>
#include <boost/json/serialize.hpp>
#include <snk/platform/plt_beneath.h>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

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
}

std::string prj::to_indented_json(const json::value& value)
{
	std::string text;
	write_indented(text, value, 0);
	text += '\n';
	return text;
}

std::optional<std::string> prj::write_text_atomically(const fs::path& link_or_file, std::string_view text)
{
	// A linked file is replaced at its target; renaming onto the link would turn it into a plain file.
	fs::path file = link_or_file;
	std::error_code link_ec;
	if (fs::is_symlink(link_or_file, link_ec)) {
		file = fs::canonical(link_or_file, link_ec);
		if (link_ec) {
			return std::format("'{}' links to a file that cannot be reached: {}", utf8_of(link_or_file), link_ec.message());
		}
	}
	std::random_device random;
	fs::path temp_name{ "." };
	temp_name += file.filename();
	temp_name += std::format(".{:08x}.tmp", random());
	const fs::path temp = file.parent_path() / temp_name;
	std::error_code ignored;
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.close();
		if (out.fail()) {
			fs::remove(temp, ignored);
			return std::format("cannot write '{}'", utf8_of(temp));
		}
	}
	std::error_code ec;
	fs::rename(temp, file, ec);
	if (ec) {
		fs::remove(temp, ignored);
		return std::format("cannot replace '{}': {}", utf8_of(file), ec.message());
	}
	return std::nullopt;
}

std::optional<std::string> prj::write_project_file(const fs::path& project_root, const fs::path& relative, std::string_view text)
{
	const fs::path shown = project_root / relative;
	const fs::path folder = relative.parent_path();
	const fs::path leaf = relative.filename();
	if (relative.has_root_path() || leaf.empty() || leaf == "." || leaf == "..") {
		return std::format("'{}' is not a file below the project root", utf8_of(shown));
	}
	// The root by its path, links resolved: only what lies below it is opened without following one.
	std::error_code ec;
	const fs::path root = fs::canonical(project_root, ec);
	if (ec) {
		return std::format("the project root '{}' cannot be opened: {}", utf8_of(project_root), ec.message());
	}

	// renameat replaces a link at the file's name instead of following it; looked at first so that a link there is an
	// error, not silently turned into a plain file.
	platform::beneath::read_request look;
	look.root = root;
	look.folder = folder;
	look.leaf = leaf;
	std::vector<std::byte> ignored;
	const platform::beneath::read_result found = platform::beneath::read_beneath(look, ignored);
	switch (found.status) {
	case platform::beneath::read_status::ok:
	case platform::beneath::read_status::too_large:
	case platform::beneath::read_status::missing:
		break;
	case platform::beneath::read_status::not_regular:
		return std::format("'{}' is a link or not a regular file; it is not written", utf8_of(shown));
	case platform::beneath::read_status::failed:
		return std::format("cannot write '{}': {}", utf8_of(shown), found.error);
	}

	std::random_device random;
	fs::path temp_leaf{ "." };
	temp_leaf += leaf;
	temp_leaf += std::format(".{:08x}.tmp", random());
	platform::beneath::pinned_root pinned{ root };
	platform::beneath::write_request request;
	request.folder = folder;
	request.leaf = leaf;
	request.tmp_leaf = temp_leaf;
	request.data = std::as_bytes(std::span<const char>(text.data(), text.size()));
	request.tmp_path = root / folder / temp_leaf;
	if (const std::string error = platform::beneath::write_beneath(pinned, request); !error.empty()) {
		return std::format("cannot write '{}': {}", utf8_of(shown), error);
	}
	return std::nullopt;
}

prj::project_file_text prj::read_project_file(const fs::path& project_root, const fs::path& relative, std::uint64_t max_bytes)
{
	project_file_text result;
	const fs::path shown = project_root / relative;
	const fs::path leaf = relative.filename();
	if (relative.has_root_path() || leaf.empty() || leaf == "." || leaf == "..") {
		result.error = std::format("'{}' is not a file below the project root", utf8_of(shown));
		return result;
	}
	std::error_code ec;
	const fs::path root = fs::canonical(project_root, ec);
	if (ec) {
		if (ec != std::errc::no_such_file_or_directory) {
			result.error = std::format("the project root '{}' cannot be opened: {}", utf8_of(project_root), ec.message());
		}
		return result;
	}

	platform::beneath::read_request request;
	request.root = root;
	request.folder = relative.parent_path();
	request.leaf = leaf;
	request.max_bytes = max_bytes;
	std::vector<std::byte> bytes;
	const platform::beneath::read_result read = platform::beneath::read_beneath(request, bytes);
	switch (read.status) {
	case platform::beneath::read_status::missing:
		return result;
	case platform::beneath::read_status::ok:
		result.exists = true;
		result.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return result;
	case platform::beneath::read_status::not_regular:
		result.exists = true;
		result.error = std::format("'{}' is a link or not a regular file; it is not read", utf8_of(shown));
		return result;
	case platform::beneath::read_status::too_large:
		result.exists = true;
		result.error = std::format("'{}' is larger than {} bytes; it is not read", utf8_of(shown), max_bytes);
		return result;
	case platform::beneath::read_status::failed:
		break;
	}
	result.exists = true;
	result.error = std::format("cannot read '{}': {}", utf8_of(shown), read.error);
	return result;
}

prj::project_file_location prj::project_file_location_of(const fs::path& file)
{
	std::error_code ec;
	fs::path absolute = fs::absolute(file, ec);
	if (ec) {
		absolute = file;
	}
	absolute = absolute.lexically_normal();
	const fs::path folder = absolute.parent_path();
	return { folder.parent_path(), folder.filename() / absolute.filename() };
}
