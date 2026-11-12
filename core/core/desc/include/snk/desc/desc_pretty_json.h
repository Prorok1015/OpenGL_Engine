#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

namespace json = boost::json;

// The one JSON text style of the project (#559): what the editor writes when it saves a level, a prefab or a desc, and what
// an import exports into the project. Four spaces per level, "\n" line ends, one at the end of the document.
namespace desc
{
	inline void write_pretty_json(std::ostream& os, json::value const& jv, std::string* indent = nullptr)
	{
		std::string indent_;
		if (!indent)
			indent = &indent_;
		switch (jv.kind())
		{
		case json::kind::object:
		{
			if (jv.get_object().empty())
			{
				os << "{}";
				break;
			}
			os << "{\n";
			indent->append(4, ' ');
			auto const& obj = jv.get_object();
			if (!obj.empty())
			{
				auto it = obj.begin();
				for (;;)
				{
					os << *indent << json::serialize(it->key()) << " : ";
					write_pretty_json(os, it->value(), indent);
					if (++it == obj.end())
						break;
					os << ",\n";
				}
			}
			os << "\n";
			indent->resize(indent->size() - 4);
			os << *indent << "}";
			break;
		}

		case json::kind::array:
		{
			auto const& arr = jv.get_array();
			// Vectors and matrices stay on one line (one number per line makes diffs unreadable); so do
			// short lists of short strings. Anything with nesting or long text gets a line per element.
			std::size_t text_length = 0;
			for (const auto& element : arr)
			{
				if (element.is_string())
					text_length += element.get_string().size() + 4;
			}
			const bool flat = text_length <= 60 &&
				std::ranges::none_of(arr, [](const json::value& v) { return v.is_array() || v.is_object(); });
			if (flat) {
				os << "[";
				indent->append(4, ' '); // keeps the scalar writes below from treating themselves as the document root
				for (auto it = arr.begin(); it != arr.end(); ++it) {
					if (it != arr.begin())
						os << ", ";
					write_pretty_json(os, *it, indent);
				}
				indent->resize(indent->size() - 4);
				os << "]";
				break;
			}
			os << "[\n";
			indent->append(4, ' ');
			if (!arr.empty())
			{
				auto it = arr.begin();
				for (;;)
				{
					os << *indent;
					write_pretty_json(os, *it, indent);
					if (++it == arr.end())
						break;
					os << ",\n";
				}
			}
			os << "\n";
			indent->resize(indent->size() - 4);
			os << *indent << "]";
			break;
		}

		case json::kind::string:
		{
			os << json::serialize(jv.get_string());
			break;
		}

		case json::kind::double_:
		{
			// Shortest text that reads back as the same double ("0.5", not "5E-1"); keep a '.' or exponent
			// so it stays a double and does not turn into an integer on the next load.
			const double number = jv.get_double();
			if (!std::isfinite(number))
			{
				os << jv;
				break;
			}
			char buffer[40];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
			const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
			os << text;
			if (text.find_first_of(".e") == std::string_view::npos)
				os << ".0";
			break;
		}

		case json::kind::uint64:
		case json::kind::int64:
			os << jv;
			break;

		case json::kind::bool_:
			if (jv.get_bool())
				os << "true";
			else
				os << "false";
			break;

		case json::kind::null:
			os << "null";
			break;
		}

		if (indent->empty())
			os << "\n";
	}

	inline std::vector<std::byte> pretty_json_bytes(const json::value& data)
	{
		std::stringstream ss;
		write_pretty_json(ss, data);
		const std::string text = ss.str();
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}
}
