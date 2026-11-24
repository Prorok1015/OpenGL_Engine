#pragma once
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace test_util {

	// A folder link at `link` that leads to `target` (which must exist): a junction on Windows (it needs no privilege), a symlink
	// elsewhere. A link that cannot be made is a broken test, never a skip: the check it stands for would be vacuous.
	inline void make_folder_link(const std::filesystem::path& link, const std::filesystem::path& target)
	{
#ifdef _WIN32
		const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
		BOOST_REQUIRE_MESSAGE(std::system(command.c_str()) == 0 && std::filesystem::exists(link),
			"mklink /J could not make a junction at " << link.string());
#else
		std::error_code ec;
		std::filesystem::create_directory_symlink(target, link, ec);
		BOOST_REQUIRE_MESSAGE(!ec, "no symlink at " << link.string() << ": " << ec.message());
#endif
	}
}
