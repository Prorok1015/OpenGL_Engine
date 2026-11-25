#pragma once
#include <filesystem>

namespace test_util {

#ifdef _WIN32
	// Sets or clears FILE_ATTRIBUTE_OFFLINE on `path`: what a file kept elsewhere (an HSM, an offline file) reports,
	// and the one attribute of read_limits::refuse_recall a test can set. Defined in test_file_attributes_windows.cpp,
	// so <windows.h> stays out of the tests.
	bool set_offline(const std::filesystem::path& path, bool offline);
#endif

}
