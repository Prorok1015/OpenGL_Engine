#pragma once
#include <snk/desc/desc_system.h>
#include <boost/json.hpp>
#include <string>
#include <string_view>

namespace desc {

	// A project template ships files of the types its game module declares (#800). A tool that loads every desc of the
	// repository without that module cannot know them: it leaves such a file out. A file of a template (a path with a
	// "templates/projects/" segment) whose root "__type" `descs` does not know.
	inline bool is_template_file_of_unknown_type(std::string_view path, std::string_view text, const desc_system& descs)
	{
		if (path.find("templates/projects/") == std::string_view::npos) {
			return false;
		}
		boost::system::error_code error;
		const boost::json::value root = boost::json::parse(text, error);
		if (error || !root.is_object()) {
			return false;
		}
		const boost::json::value* type = root.as_object().if_contains("__type");
		return type && type->is_string() && !descs.is_registered(std::string{ type->as_string() });
	}

}
