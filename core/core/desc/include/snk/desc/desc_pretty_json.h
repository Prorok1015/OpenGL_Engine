#pragma once
#include <cstddef>
#include <ostream>
#include <string>
#include <vector>
#include <boost/json/value.hpp>
#include <snk/resource/res_pretty_json.h>

namespace json = boost::json;

// Separate functions rather than a using of res:: so the names stay desc:: in a module built against 3.16; the window of
// 4.0 may drop them.
namespace desc
{
	inline void write_pretty_json(std::ostream& os, json::value const& jv, std::string* indent = nullptr)
	{
		res::write_pretty_json(os, jv, indent);
	}

	inline std::vector<std::byte> pretty_json_bytes(const json::value& data)
	{
		return res::pretty_json_bytes(data);
	}
}
