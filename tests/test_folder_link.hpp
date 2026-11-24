#pragma once
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace test_util {

	// A link to the folder `target` at `link`, as a child or a person could plant one: a junction on Windows (no
	// privilege needed, so one that cannot be made is a broken test, never a skip), a directory symlink elsewhere.
	inline bool link_folder(const std::filesystem::path& target, const std::filesystem::path& link)
	{
#ifdef _WIN32
		const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
		return std::system(command.c_str()) == 0 && std::filesystem::exists(link);
#else
		std::error_code ec;
		std::filesystem::create_directory_symlink(target, link, ec);
		return !ec;
#endif
	}

	// link_folder that fails the test when no link can be made: the check it stands for would be vacuous.
	inline void make_folder_link(const std::filesystem::path& link, const std::filesystem::path& target)
	{
		BOOST_REQUIRE_MESSAGE(link_folder(target, link), "no folder link could be made at " << link.string());
	}

}
