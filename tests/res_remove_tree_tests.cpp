#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/resource/res_shipping_dirs.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

// #1054, #1099: a run folder the decoder wrote is removed without going through what it planted.

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
// A junction `link` -> `target`, as a child could plant one (REPARSE_DATA_BUFFER spelled out: its type is in the driver
// kit's headers only).
bool make_junction(const fs::path& link, const fs::path& target)
{
	if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
	const HANDLE folder = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (folder == INVALID_HANDLE_VALUE) return false;
	const std::wstring substitute = L"\\??\\" + fs::absolute(target).wstring();
	const auto name_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
	std::vector<std::byte> buffer(8 + 8 + name_bytes + 2 * sizeof(wchar_t));
	const auto put16 = [&buffer](std::size_t at, WORD value) { std::memcpy(buffer.data() + at, &value, sizeof(value)); };
	const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
	std::memcpy(buffer.data(), &tag, sizeof(tag));
	put16(4, static_cast<WORD>(buffer.size() - 8));
	put16(8, 0);
	put16(10, name_bytes);
	put16(12, static_cast<WORD>(name_bytes + sizeof(wchar_t)));
	put16(14, 0);
	std::memcpy(buffer.data() + 16, substitute.data(), name_bytes);
	DWORD returned = 0;
	const BOOL set = DeviceIoControl(folder, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0,
		&returned, nullptr);
	CloseHandle(folder);
	return set != FALSE;
}

// READONLY set on `path` itself, a reparse point included.
bool set_read_only(const fs::path& path)
{
	const HANDLE entry = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (entry == INVALID_HANDLE_VALUE) return false;
	FILE_BASIC_INFO basic{};
	bool set = GetFileInformationByHandleEx(entry, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
	basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
	set = set && SetFileInformationByHandle(entry, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
	CloseHandle(entry);
	return set;
}
#endif

}

BOOST_AUTO_TEST_SUITE(ResRemoveTreeTests)

BOOST_AUTO_TEST_CASE(ALinkInTheTree_IsRemoved_WhatItPointsAtStays)
{
	test_util::temp_dir dir{ "remove_tree_link" };
	const fs::path kept = dir.write("outside/keep.txt", "the user's");
	dir.write("tree/sub/file.txt", "the child's");
#if defined(_WIN32)
	BOOST_REQUIRE(make_junction(dir.path() / "tree" / "escape", dir.path() / "outside"));
#else
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "tree" / "escape");
#endif
	std::error_code ec;
	BOOST_CHECK_MESSAGE(core::res::remove_tree_without_following(dir.path() / "tree", ec), ec.message());
	BOOST_CHECK(!fs::exists(dir.path() / "tree"));
	BOOST_CHECK(fs::exists(kept));
}

#if defined(_WIN32)
BOOST_AUTO_TEST_CASE(AReadOnlyJunction_IsRemoved_AndItsReadOnlyTargetIsLeftAsItWas)
{
	test_util::temp_dir dir{ "remove_tree_read_only" };
	const fs::path kept = dir.write("outside/keep.txt", "the user's");
	const fs::path target = dir.path() / "outside";
	BOOST_REQUIRE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY));
	fs::create_directories(dir.path() / "tree");
	const fs::path link = dir.path() / "tree" / "escape";
	BOOST_REQUIRE(make_junction(link, target));
	BOOST_REQUIRE(set_read_only(link));

	std::error_code ec;
	BOOST_CHECK_MESSAGE(core::res::remove_tree_without_following(dir.path() / "tree", ec), ec.message());
	BOOST_CHECK(!fs::exists(dir.path() / "tree"));
	BOOST_CHECK(fs::exists(kept));
	const DWORD attributes = GetFileAttributesW(target.c_str());
	BOOST_CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0);
	SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_DIRECTORY);
}

BOOST_AUTO_TEST_CASE(ATreeDeeperThanMaxPath_IsRemovedWhole)
{
	test_util::temp_dir dir{ "remove_tree_long" };
	std::wstring deep = L"\\\\?\\" + (dir.path() / "tree").lexically_normal().make_preferred().wstring();
	while (deep.size() < MAX_PATH + 60) deep += L"\\a_folder_name_of_some_length";
	std::error_code made;
	fs::create_directories(fs::path(deep), made);
	BOOST_REQUIRE_MESSAGE(!made, made.message());
	const HANDLE file = CreateFileW((deep + L"\\file.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_READONLY, nullptr);
	BOOST_REQUIRE(file != INVALID_HANDLE_VALUE);
	CloseHandle(file);

	std::error_code ec;
	BOOST_CHECK_MESSAGE(core::res::remove_tree_without_following(dir.path() / "tree", ec), ec.message());
	BOOST_CHECK(!fs::exists(dir.path() / "tree"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
