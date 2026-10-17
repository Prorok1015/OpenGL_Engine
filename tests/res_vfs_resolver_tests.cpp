#include <boost/test/unit_test.hpp>
#include "resolvers/res_vfs_resolver.h"
#include "test_temp_dir.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

std::string to_string(const std::vector<std::byte>& b)
{
	std::string s(b.size(), '\0');
	for (size_t i = 0; i < b.size(); ++i) s[i] = static_cast<char>(b[i]);
	return s;
}

// Blocks on the async result; returns the payload text, or "ERR:<msg>" on failure.
std::string load(const res::vfs_resolver& r, const char* tag)
{
	auto block = r.resolve(res::tag{ tag });
	block->wait();
	if (block->has_error()) return "ERR:" + block->error_msg;
	return to_string(block->data);
}

bool wait_for_file(const std::filesystem::path& p)
{
	for (int i = 0; i < 500; ++i) {
		if (std::filesystem::exists(p)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

// Mount order matters: the LAST point is the top of the stack and wins.
struct mounts_fixture
{
	test_util::temp_dir dir{ "vfs" };
	std::filesystem::path low = dir.path() / "low";
	std::filesystem::path high = dir.path() / "high";

	mounts_fixture()
	{
		dir.write("low/a.txt", "low-a");
		dir.write("low/only_low.txt", "low-only");
		dir.write("low/sub/deep.txt", "low-deep");
		dir.write("high/a.txt", "high-a");
		dir.write("high/only_high.txt", "high-only");
	}
};

}

BOOST_AUTO_TEST_SUITE(ResVfsResolverTests)

BOOST_FIXTURE_TEST_CASE(Resolve_TopMountShadowsLowerMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://a.txt") == "high-a");
}

BOOST_FIXTURE_TEST_CASE(Resolve_FallsThroughToLowerMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(load(r, "res://only_low.txt") == "low-only");
	BOOST_TEST(load(r, "res://only_high.txt") == "high-only");
	BOOST_TEST(load(r, "res://sub/deep.txt") == "low-deep");
}

BOOST_FIXTURE_TEST_CASE(Resolve_MountOrderDecidesPriority, mounts_fixture)
{
	res::vfs_resolver r("res", { high, low });
	BOOST_TEST(load(r, "res://a.txt") == "low-a");
}

BOOST_FIXTURE_TEST_CASE(Resolve_MissingFileReportsErrorInsteadOfThrowing, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto result = load(r, "res://nope.txt");
	BOOST_TEST(result.rfind("ERR:", 0) == 0u);
	BOOST_TEST(result.find("does not exist") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(Resolve_EmptyFileIsReadyWithNoBytes, mounts_fixture)
{
	dir.write("high/empty.bin", "");
	res::vfs_resolver r("res", { low, high });
	auto block = r.resolve(res::tag{ "res://empty.bin" });
	block->wait();
	BOOST_TEST(block->is_ready());
	BOOST_TEST(block->data.empty());
}

BOOST_AUTO_TEST_CASE(Resolve_WithoutMountsReportsNoEntryPoints)
{
	res::vfs_resolver r("res", std::vector<std::filesystem::path>{});
	auto result = load(r, "res://a.txt");
	BOOST_TEST(result.find("No entry points") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(Exists_SearchesEveryMount, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.exists(res::tag{ "res://a.txt" }));
	BOOST_TEST(r.exists(res::tag{ "res://only_low.txt" }));
	BOOST_TEST(r.exists(res::tag{ "res://only_high.txt" }));
	BOOST_TEST(!r.exists(res::tag{ "res://missing.txt" }));
}

BOOST_FIXTURE_TEST_CASE(Store_WritesToTopMountOnlyAndCreatesFolders, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.store(res::tag{ "res://gen/out/new.bin" }, to_bytes("payload")));

	// readable right away (served from the pending-write table or from disk)
	BOOST_TEST(load(r, "res://gen/out/new.bin") == "payload");

	BOOST_TEST(wait_for_file(high / "gen/out/new.bin"));
	BOOST_TEST(!std::filesystem::exists(low / "gen/out/new.bin"));
	// the atomic-rename temp file is gone
	BOOST_TEST(!std::filesystem::exists(high / "gen/out/new.tmp"));
}

BOOST_FIXTURE_TEST_CASE(Store_OverwritesShadowedLowerMountFile, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(r.store(res::tag{ "res://only_low.txt" }, to_bytes("v2")));
	BOOST_TEST(wait_for_file(high / "only_low.txt"));
	BOOST_TEST(load(r, "res://only_low.txt") == "v2");
	// the lower mount is not modified
	BOOST_TEST(load(res::vfs_resolver("res", { low }), "res://only_low.txt") == "low-only");
}

BOOST_FIXTURE_TEST_CASE(Store_ThenExistsIsTrueImmediately, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.exists(res::tag{ "res://later.txt" }));
	r.store(res::tag{ "res://later.txt" }, to_bytes("x"));
	BOOST_TEST(r.exists(res::tag{ "res://later.txt" }));
}

BOOST_AUTO_TEST_CASE(Store_WithoutMountsFailsButNotifiesChange)
{
	res::vfs_resolver r("res", std::vector<std::filesystem::path>{});
	std::atomic<int> notified{ 0 };
	std::string last;
	r.set_resource_changed_callback([&](const res::tag& t) {
		++notified;
		last = std::string(t.view());
	});
	BOOST_TEST(!r.store(res::tag{ "res://x.txt" }, to_bytes("abc")));
	BOOST_TEST(notified.load() == 1);
	BOOST_TEST(last == "res://x.txt");
	// the data is still served from memory even though nothing was persisted
	BOOST_TEST(load(r, "res://x.txt") == "abc");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_MapsFilesUnderAnyMountToTags, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto t1 = r.path_mapper(high / "a.txt");
	BOOST_REQUIRE(t1.has_value());
	BOOST_TEST(t1->protocol() == "res");
	BOOST_TEST(t1->relative() == "a.txt");

	auto t2 = r.path_mapper(low / "sub" / "deep.txt");
	BOOST_REQUIRE(t2.has_value());
	BOOST_TEST(t2->relative() == "sub/deep.txt");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_NormalizesDotSegments, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	auto t = r.path_mapper(high / "x" / ".." / "a.txt");
	BOOST_REQUIRE(t.has_value());
	BOOST_TEST(t->relative() == "a.txt");
}

BOOST_FIXTURE_TEST_CASE(PathMapper_RejectsPathsOutsideAllMounts, mounts_fixture)
{
	res::vfs_resolver r("res", { low, high });
	BOOST_TEST(!r.path_mapper(dir.path() / "elsewhere" / "a.txt").has_value());
	BOOST_TEST(!r.path_mapper(dir.path() / "low_sibling" / "a.txt").has_value());
}

BOOST_FIXTURE_TEST_CASE(PathMapper_NestedMountsPreferTopOfStack, mounts_fixture)
{
	// "inner" lives inside "outer"; inner is mounted last, so it owns its subtree.
	auto outer = dir.path() / "outer";
	auto inner = outer / "inner";
	std::filesystem::create_directories(inner);
	res::vfs_resolver r("res", { outer, inner });

	auto in_inner = r.path_mapper(inner / "f.txt");
	BOOST_REQUIRE(in_inner.has_value());
	BOOST_TEST(in_inner->relative() == "f.txt");

	auto in_outer = r.path_mapper(outer / "g.txt");
	BOOST_REQUIRE(in_outer.has_value());
	BOOST_TEST(in_outer->relative() == "g.txt");
}

BOOST_FIXTURE_TEST_CASE(ExternalFileChange_FiresResourceChangedCallback, mounts_fixture)
{
	std::atomic<bool> fired{ false };
	std::string seen;
	std::mutex m;
	res::vfs_resolver r("res", { low, high });
	r.set_resource_changed_callback([&](const res::tag& t) {
		std::lock_guard lock(m);
		seen = std::string(t.relative());
		fired = true;
	});
	// give the watcher a chance to take its initial snapshot, then add a file externally
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	dir.write("high/created_externally.txt", "hi");
	for (int i = 0; i < 300 && !fired; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	BOOST_TEST(fired.load());
	std::lock_guard lock(m);
	BOOST_TEST(seen == "created_externally.txt");
}

BOOST_AUTO_TEST_SUITE_END()
