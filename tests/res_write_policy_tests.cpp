#include <boost/test/unit_test.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/resolvers/res_write_policy.h>
#include "test_temp_dir.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// Which paths a resolver may store into (res::write_policy) and its use by vfs_resolver::store.

namespace {

res::write_policy edt_policy(std::vector<std::string> patterns)
{
	return res::write_policy{ "edt", patterns };
}

std::vector<std::byte> bytes_of(std::string_view text)
{
	std::vector<std::byte> out(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
	return out;
}

bool wait_for_file(const std::filesystem::path& file)
{
	for (int i = 0; i < 500; ++i) {
		if (std::filesystem::exists(file)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

}

BOOST_AUTO_TEST_SUITE(ResWritePolicy)

BOOST_AUTO_TEST_CASE(AllowAll_AllowsAnyPath_ReadOnly_AllowsNone)
{
	BOOST_TEST(res::write_policy::allow_all().allows("a/b/c.txt"));
	BOOST_TEST(!res::write_policy::read_only().allows("a/b/c.txt"));
	BOOST_TEST(!edt_policy({}).allows("settings/a.ini"));
}

BOOST_AUTO_TEST_CASE(DoubleStarSegment_ReachesAnyDepthUnderItsPrefix)
{
	const auto policy = edt_policy({ "edt://settings/**" });

	BOOST_TEST(policy.allows("settings/layout.ini"));
	BOOST_TEST(policy.allows("settings/profiles/a/b.json"));
	BOOST_TEST(!policy.allows("icons/logo.png"));
}

BOOST_AUTO_TEST_CASE(NeighbouringFolderWithTheSamePrefix_IsNotAllowed)
{
	const auto policy = edt_policy({ "edt://settings/**" });

	BOOST_TEST(!policy.allows("settings_evil/x"));
	BOOST_TEST(!policy.allows("my_settings/x"));
}

BOOST_AUTO_TEST_CASE(SingleStar_DoesNotCrossASegmentBoundary)
{
	const auto policy = edt_policy({ "edt://settings/*" });

	BOOST_TEST(policy.allows("settings/a.ini"));
	BOOST_TEST(!policy.allows("settings/sub/a.ini"));
	BOOST_TEST(!policy.allows("settings"));
}

BOOST_AUTO_TEST_CASE(SingleStar_MatchesInsideASegment)
{
	const auto policy = edt_policy({ "edt://settings/*.ini", "edt://cache/a*c" });

	BOOST_TEST(policy.allows("settings/layout.ini"));
	BOOST_TEST(!policy.allows("settings/layout.json"));
	BOOST_TEST(policy.allows("cache/abc"));
	BOOST_TEST(policy.allows("cache/ac"));
	BOOST_TEST(!policy.allows("cache/abcd"));
}

BOOST_AUTO_TEST_CASE(DoubleStarInTheMiddle_MatchesZeroOrMoreSegments)
{
	const auto policy = edt_policy({ "edt://a/**/z.txt" });

	BOOST_TEST(policy.allows("a/z.txt"));
	BOOST_TEST(policy.allows("a/b/z.txt"));
	BOOST_TEST(policy.allows("a/b/c/z.txt"));
	BOOST_TEST(!policy.allows("a/b/y.txt"));
}

BOOST_AUTO_TEST_CASE(Matching_IsCaseSensitive)
{
	const auto policy = edt_policy({ "edt://settings/**" });

	BOOST_TEST(!policy.allows("Settings/a.ini"));
}

BOOST_AUTO_TEST_CASE(PatternsOfAnotherProtocol_AreSkipped)
{
	const auto policy = edt_policy({ "res://**", "edt://settings/**" });

	BOOST_TEST(!policy.allows("icons/logo.png"));
	BOOST_TEST(policy.allows("settings/a.ini"));
	BOOST_TEST(res::write_policy("res", { "res://**" }).allows("anything/at/all"));
}

BOOST_AUTO_TEST_CASE(MalformedPatterns_AllowNothing)
{
	for (const char* bad : { "settings/**", "edt://", "edt://a//b", "edt://a/", "edt://a**/b", "edt://**x/b", "" }) {
		BOOST_TEST(!edt_policy({ bad }).allows("a/b"), bad);
		BOOST_TEST(!edt_policy({ bad }).allows("settings/a"), bad);
	}
}

BOOST_AUTO_TEST_CASE(EmptyPath_IsNeverAllowed)
{
	BOOST_TEST(!edt_policy({ "edt://**" }).allows(""));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(VfsResolverWritePolicy)

BOOST_AUTO_TEST_CASE(Store_AllowedPath_IsWritten)
{
	test_util::temp_dir dir{ "write_policy_allowed" };
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(resolver.store(res::tag{ "edt://settings/layout.ini" }, bytes_of("x")));
		BOOST_TEST(wait_for_file(dir.path() / "settings" / "layout.ini"));
	}
}

BOOST_AUTO_TEST_CASE(Store_PathOutsideThePatterns_IsRefusedAndNothingIsWritten)
{
	test_util::temp_dir dir{ "write_policy_refused" };
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(!resolver.store(res::tag{ "edt://icons/x.png" }, bytes_of("x")));
		BOOST_TEST(!resolver.store(res::tag{ "edt://settings_evil/x" }, bytes_of("x")));
		BOOST_TEST(!resolver.store(res::tag{ "edt://settings/../icons/x.png" }, bytes_of("x")));
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	BOOST_TEST(!std::filesystem::exists(dir.path() / "icons"));
	BOOST_TEST(!std::filesystem::exists(dir.path() / "settings_evil"));
}

BOOST_AUTO_TEST_CASE(Store_TagInTheWrongCase_IsRefused)
{
	test_util::temp_dir dir{ "write_policy_case" };
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(!resolver.store(res::tag{ "edt://Settings/a.ini" }, bytes_of("x")));
		BOOST_TEST(!resolver.store(res::tag{ "edt://SETTINGS/a.ini" }, bytes_of("x")));
	}
}

BOOST_AUTO_TEST_CASE(Store_PatternsOfAnotherProtocol_DoNotOpenThisOne)
{
	test_util::temp_dir dir{ "write_policy_other_protocol" };
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "res://**", "engine://settings/**" }) };

		BOOST_TEST(!resolver.store(res::tag{ "edt://settings/a.ini" }, bytes_of("x")));
	}
}

#ifndef _WIN32
// safe_path refuses a link as the last component, so a link can only matter through a linked folder in the path:
// the tag then names a writable place while the file lands in another folder of the same root.
namespace {

std::string content_of(const std::filesystem::path& file)
{
	std::ifstream in(file);
	return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

}

BOOST_AUTO_TEST_CASE(Store_ThroughALinkedFolderIntoAFolderTheListDoesNotCover_IsRefusedByThePolicy)
{
	test_util::temp_dir dir{ "write_policy_linked_folder" };
	dir.write("icons/real.txt", "shipped");
	std::filesystem::create_directories(dir.path() / "settings");
	std::filesystem::create_symlink("../icons", dir.path() / "settings" / "linked");
	const std::vector<std::filesystem::path> roots{ dir.path() };
	{
		// Control: without a policy safe_path accepts this tag, so the refusal below comes from the policy.
		res::vfs_resolver open{ "edt", roots };
		BOOST_REQUIRE(open.store(res::tag{ "edt://settings/linked/probe.txt" }, bytes_of("probe")));
		BOOST_REQUIRE(wait_for_file(dir.path() / "icons" / "probe.txt"));
	}
	{
		res::vfs_resolver resolver{ "edt", roots, res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(!resolver.store(res::tag{ "edt://settings/linked/real.txt" }, bytes_of("overwritten")));
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	BOOST_TEST(content_of(dir.path() / "icons" / "real.txt") == "shipped");
}

BOOST_AUTO_TEST_CASE(Store_ThroughALinkedFolderInsideTheList_IsAccepted)
{
	test_util::temp_dir dir{ "write_policy_linked_inside" };
	dir.write("settings/inside/keep.txt", "x");
	std::filesystem::create_symlink("inside", dir.path() / "settings" / "alias");
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(resolver.store(res::tag{ "edt://settings/alias/new.txt" }, bytes_of("x")));
		BOOST_TEST(wait_for_file(dir.path() / "settings" / "inside" / "new.txt"));
	}
}

BOOST_AUTO_TEST_CASE(Store_ALinkAsTheLastComponent_IsRefusedWhereverItLeads)
{
	test_util::temp_dir dir{ "write_policy_leaf_link" };
	dir.write("icons/real.txt", "shipped");
	dir.write("settings/inside/real.txt", "mine");
	std::filesystem::create_symlink("../icons/real.txt", dir.path() / "settings" / "to_icons.txt");
	std::filesystem::create_symlink("inside/real.txt", dir.path() / "settings" / "to_inside.txt");
	{
		res::vfs_resolver resolver{ "edt", std::vector<std::filesystem::path>{ dir.path() },
			res::mount_access::read_write, edt_policy({ "edt://settings/**" }) };

		BOOST_TEST(!resolver.store(res::tag{ "edt://settings/to_icons.txt" }, bytes_of("overwritten")));
		BOOST_TEST(!resolver.store(res::tag{ "edt://settings/to_inside.txt" }, bytes_of("updated")));
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	BOOST_TEST(content_of(dir.path() / "icons" / "real.txt") == "shipped");
	BOOST_TEST(content_of(dir.path() / "settings" / "inside" / "real.txt") == "mine");
}
#endif

BOOST_AUTO_TEST_CASE(Store_ReadOnlyPolicy_RefusesEverythingButStillResolvesFiles)
{
	test_util::temp_dir dir{ "write_policy_read_only" };
	dir.write("a.txt", "shipped");
	res::vfs_resolver resolver{ "engine", std::vector<std::filesystem::path>{ dir.path() }, res::mount_access::read_write, res::write_policy::read_only() };

	BOOST_TEST(!resolver.store(res::tag{ "engine://b.txt" }, bytes_of("x")));
	BOOST_TEST(resolver.exists(res::tag{ "engine://a.txt" }));
	auto block = resolver.resolve(res::tag{ "engine://a.txt" });
	block->wait();
	BOOST_TEST(!block->has_error());
}

BOOST_AUTO_TEST_CASE(Store_WithoutAPolicy_StillWritesAnywhere)
{
	test_util::temp_dir dir{ "write_policy_default" };
	{
		res::vfs_resolver resolver{ "res", std::vector<std::filesystem::path>{ dir.path() } };

		BOOST_TEST(resolver.store(res::tag{ "res://any/where.txt" }, bytes_of("x")));
		BOOST_TEST(wait_for_file(dir.path() / "any" / "where.txt"));
	}
}

BOOST_AUTO_TEST_SUITE_END()
