#include <boost/test/unit_test.hpp>
#include "test_file_attributes.hpp"
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// #1412: read_beneath with read_limits, the read of an agent's import: no mount crossed below the root, nothing a read
// would fetch from the network.

namespace fs = std::filesystem;

namespace {

platform::beneath::read_request request_for(const fs::path& root, const fs::path& relative)
{
	platform::beneath::read_request request;
	request.root = root;
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = 1u << 20;
	return request;
}

platform::beneath::read_limits all_limits()
{
	platform::beneath::read_limits limits;
	limits.same_device = true;
	limits.refuse_recall = true;
	return limits;
}

bool names(const std::string& error, std::string_view what)
{
	return error.find(what) != std::string::npos;
}

}

BOOST_AUTO_TEST_SUITE(PltBeneathLimits)

BOOST_AUTO_TEST_CASE(a_plain_file_on_the_roots_device_reads_as_without_limits)
{
	test_util::temp_dir dir("beneath_limits_plain");
	dir.write("a/b/robot.glb", "glTF");
	const fs::path root = fs::weakly_canonical(dir.path());
	std::vector<std::byte> bytes;
	const auto read = platform::beneath::read_beneath(request_for(root, "a/b/robot.glb"), all_limits(), bytes);
	BOOST_REQUIRE_MESSAGE(read.status == platform::beneath::read_status::ok, read.error);
	BOOST_TEST(bytes.size() == 4u);
	BOOST_TEST(read.stamp.has_value());
}

#if defined(__linux__)
// /proc is a mount of its own below "/": the walk stops at it.
BOOST_AUTO_TEST_CASE(a_folder_of_another_device_below_the_root_is_refused)
{
	std::vector<std::byte> bytes;
	const auto crossed = platform::beneath::read_beneath(request_for("/", "proc/version"), all_limits(), bytes);
	BOOST_TEST((crossed.status == platform::beneath::read_status::failed));
	BOOST_CHECK_MESSAGE(names(crossed.error, platform::beneath::k_other_device), crossed.error);
	BOOST_TEST(bytes.empty());

	platform::beneath::read_limits recall_only;
	recall_only.refuse_recall = true;
	const auto read = platform::beneath::read_beneath(request_for("/", "proc/version"), recall_only, bytes);
	BOOST_CHECK_MESSAGE(read.status == platform::beneath::read_status::ok, read.error);
}
#endif

#if defined(_WIN32)
// An offline file is refused before a byte of it is read; the same file without the attribute reads.
BOOST_AUTO_TEST_CASE(a_file_a_read_would_fetch_is_refused)
{
	test_util::temp_dir dir("beneath_limits_offline");
	dir.write("a/robot.glb", "glTF");
	const fs::path root = fs::weakly_canonical(dir.path());
	BOOST_REQUIRE(test_util::set_offline(root / "a" / "robot.glb", true));
	std::vector<std::byte> bytes;
	const auto refused = platform::beneath::read_beneath(request_for(root, "a/robot.glb"), all_limits(), bytes);
	BOOST_TEST((refused.status == platform::beneath::read_status::failed));
	BOOST_CHECK_MESSAGE(names(refused.error, platform::beneath::k_would_recall), refused.error);
	BOOST_TEST(bytes.empty());

	const auto plain = platform::beneath::read_beneath(request_for(root, "a/robot.glb"), bytes);
	BOOST_CHECK_MESSAGE(plain.status == platform::beneath::read_status::ok, plain.error);

	// A folder on the way with the attribute is refused by its handle.
	BOOST_REQUIRE(test_util::set_offline(root / "a" / "robot.glb", false));
	BOOST_REQUIRE(test_util::set_offline(root / "a", true));
	const auto folder = platform::beneath::read_beneath(request_for(root, "a/robot.glb"), all_limits(), bytes);
	BOOST_CHECK_MESSAGE(names(folder.error, platform::beneath::k_would_recall), folder.error);
	BOOST_REQUIRE(test_util::set_offline(root / "a", false));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
