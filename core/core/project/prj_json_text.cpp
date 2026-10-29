#include <snk/project/prj_json_text.h>
#include <cstddef>
#include <format>
#include <fstream>
#include <random>
#include <system_error>
#include <boost/json/serialize.hpp>

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
