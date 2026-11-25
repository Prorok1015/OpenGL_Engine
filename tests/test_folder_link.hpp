#pragma once
#include <boost/test/unit_test.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace test_util {

#ifdef _WIN32
	// `link` made a junction (IO_REPARSE_TAG_MOUNT_POINT) to `target`, no shell and no privilege. Defined in
	// test_folder_link_windows.cpp, so <windows.h> and its macros (OPAQUE, TRANSPARENT...) stay out of the tests.
	bool make_junction(const std::filesystem::path& target, const std::filesystem::path& link);
#endif

	// A link to the folder `target` at `link`, as a child or a person could plant one: a junction on Windows (no
	// privilege needed, so one that cannot be made is a broken test, never a skip), a directory symlink elsewhere.
	// `target` need not exist.
	inline bool link_folder(const std::filesystem::path& target, const std::filesystem::path& link)
	{
#ifdef _WIN32
		return make_junction(target, link);
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
