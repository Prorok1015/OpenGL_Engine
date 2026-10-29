#include <snk/project/prj_json_text.h>
#include <cstddef>
#include <boost/json/serialize.hpp>

namespace json = boost::json;

namespace
{
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
