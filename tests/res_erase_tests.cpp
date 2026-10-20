#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/resource/resources/res_resource_text.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <memory>
#include <string>
#include <vector>

// resource_system::erase: the data, the cache entries, the alias mappings that point at the tag, then a change signal.
// Manual task_system: watchers are called when the test pumps.

namespace {

// A resolver that keeps the default erase (refuses).
class keep_resolver : public core::res::res_resolver_interface
{
public:
	explicit keep_resolver(const std::string_view prt) : res_resolver_interface(prt) {}
	async_raw_data resolve(const res::tag&) const override { return {}; }
};

struct erase_fixture : test_util::ram_res_env
{
	~erase_fixture()
	{
		pump(); // a queued delivery must not call into the members below
		if (owner) {
			for (const auto& tag : watched) {
				rs->unwatch(tag, owner.get());
			}
		}
	}

	std::string text(const res::tag& tag)
	{
		auto handle = rs->require_sync<res::text_resource>(tag);
		auto ptr = handle.get_sync();
		return ptr ? ptr->str() : std::string("<null>");
	}

	void watch(const res::tag& tag)
	{
		if (!owner) {
			owner = std::make_unique<int>(0);
		}
		watched.push_back(tag);
		rs->watch(tag, owner.get(), [this](const res::tag& changed) { delivered.push_back(changed.string()); });
	}

	std::unique_ptr<int> owner;
	std::vector<res::tag> watched;
	std::vector<std::string> delivered;
};

}

BOOST_FIXTURE_TEST_SUITE(ResEraseTests, erase_fixture)

BOOST_AUTO_TEST_CASE(Erase_RemovesTheDataFromTheResolver)
{
	const auto tag = put("gone.txt", "hello");
	BOOST_TEST(text(tag) == "hello");

	BOOST_TEST(rs->erase(tag));

	BOOST_TEST(!rs->exists(tag));
	BOOST_TEST(!ram->files.contains(tag));
	auto handle = rs->require_sync<res::text_resource>(tag);
	BOOST_TEST(handle.get_sync() == nullptr);
}

BOOST_AUTO_TEST_CASE(Erase_DropsTheCacheEntry_StoreAfterwardsLoadsTheNewData)
{
	const auto tag = put("cached.txt", "old");
	BOOST_TEST(text(tag) == "old");

	BOOST_TEST(rs->erase(tag));
	rs->store(tag, test_util::to_bytes("new"));

	BOOST_TEST(text(tag) == "new");
}

BOOST_AUTO_TEST_CASE(Erase_NotifiesTheWatchersOfTheTag)
{
	const auto tag = put("watched.txt", "x");
	BOOST_TEST(text(tag) == "x");
	watch(tag);

	BOOST_TEST(rs->erase(tag));
	BOOST_TEST(delivered.empty());
	pump();

	BOOST_REQUIRE_EQUAL(delivered.size(), 1u);
	BOOST_TEST(delivered.front() == tag.string());
}

BOOST_AUTO_TEST_CASE(Erase_OfATagThatIsNotThere_ReturnsFalseAndSignalsNothing)
{
	const auto tag = tag_of("missing.txt");
	watch(tag);

	BOOST_TEST(!rs->erase(tag));
	pump();

	BOOST_TEST(delivered.empty());
}

BOOST_AUTO_TEST_CASE(Erase_OfAnAliasTarget_DropsTheMappingAndTheEdge)
{
	const auto original = put("model.txt", "src");
	const auto generated = put("model_generated.txt", "gen");
	rs->register_alias(original, generated);
	BOOST_REQUIRE(rs->resolve_alias(original) == generated);
	BOOST_REQUIRE_EQUAL(rs->alias_edge_count(), 1u);

	BOOST_TEST(rs->erase(generated));

	BOOST_TEST(rs->resolve_alias(original) == original);
	BOOST_TEST(rs->alias_edge_count() == 0u);
	BOOST_TEST(text(original) == "src"); // the original keeps its own data
}

BOOST_AUTO_TEST_CASE(Erase_OfAnOriginal_DropsItsAliasMappingAndNotifiesTheAlias)
{
	const auto original = put("model.txt", "src");
	const auto generated = put("model_generated.txt", "gen");
	BOOST_TEST(text(original) == "src");
	rs->register_alias(original, generated);
	watch(generated);

	BOOST_TEST(rs->erase(original));
	pump();

	BOOST_TEST(rs->resolve_alias(original) == original);
	BOOST_TEST(rs->alias_edge_count() == 0u);
	BOOST_TEST(delivered.size() == 1u); // the alias depends on the original, so it hears of the change
}

BOOST_AUTO_TEST_CASE(Erase_LeavesTheOtherAliasesAlone)
{
	const auto a = put("a.txt", "a");
	const auto a_alias = put("a_alias.txt", "aa");
	const auto b = put("b.txt", "b");
	const auto b_alias = put("b_alias.txt", "bb");
	rs->register_alias(a, a_alias);
	rs->register_alias(b, b_alias);

	BOOST_TEST(rs->erase(a_alias));

	BOOST_TEST(rs->resolve_alias(a) == a);
	BOOST_TEST(rs->resolve_alias(b) == b_alias);
	BOOST_TEST(rs->alias_edge_count() == 1u);
}

BOOST_AUTO_TEST_CASE(Erase_OnAResolverThatCannot_ReturnsFalse)
{
	const auto tag = tag_of("kept.txt");
	rs->unregistrate_resolver(res::tag::memory);
	rs->registrate_resolver<keep_resolver>(res::tag::memory);

	BOOST_TEST(!rs->erase(tag));
}

BOOST_AUTO_TEST_SUITE_END()

// The vfs resolver over a real directory: the memory protocol erases, the files go on the io thread.

namespace {

bool wait_until_gone(const std::filesystem::path& file)
{
	for (int i = 0; i < 500; ++i) {
		if (!std::filesystem::exists(file)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

bool wait_until_there(const std::filesystem::path& file)
{
	for (int i = 0; i < 500; ++i) {
		if (std::filesystem::exists(file)) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

struct vfs_erase_fixture
{
	vfs_erase_fixture()
	{
		root = std::filesystem::temp_directory_path() / "snk_res_erase_vfs_tests";
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
		std::filesystem::create_directories(root);
	}

	~vfs_erase_fixture()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}

	std::filesystem::path root;
};

}

BOOST_FIXTURE_TEST_SUITE(VfsResolverEraseTests, vfs_erase_fixture)

BOOST_AUTO_TEST_CASE(Erase_OfATagItDoesNotHold_ReturnsFalse)
{
	res::vfs_resolver memory{ res::tag::memory, std::vector<std::filesystem::path>{ root } };

	BOOST_TEST(!memory.erase(res::tag{ "memory://nothing/here.txt" }));
}

BOOST_AUTO_TEST_CASE(Erase_OfAStoredTag_HidesItAtOnceAndRemovesTheFile)
{
	res::vfs_resolver memory{ res::tag::memory, std::vector<std::filesystem::path>{ root } };
	const res::tag tag{ "memory://m/a.txt" };
	BOOST_REQUIRE(memory.store(tag, test_util::to_bytes("x")));
	BOOST_REQUIRE(wait_until_there(root / "m" / "a.txt"));

	BOOST_TEST(memory.erase(tag));
	BOOST_TEST(!memory.exists(tag));
	BOOST_TEST(wait_until_gone(root / "m" / "a.txt"));
	BOOST_TEST(!memory.exists(tag));
}

BOOST_AUTO_TEST_CASE(StoreEraseStoreErase_EndsWithTheFileGone)
{
	res::vfs_resolver memory{ res::tag::memory, std::vector<std::filesystem::path>{ root } };
	const res::tag tag{ "memory://m/b.txt" };

	BOOST_REQUIRE(memory.store(tag, test_util::to_bytes("1")));
	BOOST_REQUIRE(memory.erase(tag));
	BOOST_REQUIRE(memory.store(tag, test_util::to_bytes("2")));
	BOOST_REQUIRE(memory.erase(tag));

	BOOST_TEST(!memory.exists(tag));
	// a sentinel stored after the last erase is saved behind every queued task
	const res::tag sentinel{ "memory://m/sentinel.txt" };
	BOOST_REQUIRE(memory.store(sentinel, test_util::to_bytes("s")));
	BOOST_REQUIRE(wait_until_there(root / "m" / "sentinel.txt"));
	BOOST_TEST(!std::filesystem::exists(root / "m" / "b.txt"));
	BOOST_TEST(!memory.exists(tag));
}

BOOST_AUTO_TEST_CASE(Store_AfterErase_KeepsTheNewFile)
{
	res::vfs_resolver memory{ res::tag::memory, std::vector<std::filesystem::path>{ root } };
	const res::tag tag{ "memory://m/c.txt" };
	BOOST_REQUIRE(memory.store(tag, test_util::to_bytes("1")));
	BOOST_REQUIRE(memory.erase(tag));
	BOOST_REQUIRE(memory.store(tag, test_util::to_bytes("2")));

	BOOST_TEST(memory.exists(tag));
	BOOST_REQUIRE(wait_until_there(root / "m" / "c.txt"));
	const res::tag sentinel{ "memory://m/sentinel.txt" };
	BOOST_REQUIRE(memory.store(sentinel, test_util::to_bytes("s")));
	BOOST_REQUIRE(wait_until_there(root / "m" / "sentinel.txt"));
	BOOST_TEST(std::filesystem::exists(root / "m" / "c.txt"));
	BOOST_TEST(memory.exists(tag));
}

BOOST_AUTO_TEST_CASE(Erase_OfATagThatEscapesTheRoot_ReturnsFalseAndKeepsTheFileOutside)
{
	res::vfs_resolver memory{ res::tag::memory, std::vector<std::filesystem::path>{ root } };
	const auto outside = root.parent_path() / "snk_res_erase_outside.txt";
	{
		std::ofstream file(outside, std::ios::binary);
		file << "keep";
	}
	const res::tag dotdot{ "memory://../snk_res_erase_outside.txt" };
	const res::tag absolute{ res::tag::memory, outside.generic_string() };

	BOOST_TEST(!memory.erase(dotdot));
	BOOST_TEST(!memory.erase(absolute));

	// a sentinel behind the queue: if an erase task had been posted it has run by now
	const res::tag sentinel{ "memory://m/sentinel.txt" };
	BOOST_REQUIRE(memory.store(sentinel, test_util::to_bytes("s")));
	BOOST_REQUIRE(wait_until_there(root / "m" / "sentinel.txt"));
	const bool kept = std::filesystem::exists(outside);
	std::error_code ignored;
	std::filesystem::remove(outside, ignored);
	BOOST_TEST(kept);
}

BOOST_AUTO_TEST_CASE(Erase_OnAnotherProtocol_ReturnsFalseAndKeepsTheFile)
{
	res::vfs_resolver layer{ "res", std::vector<std::filesystem::path>{ root } };
	const res::tag tag{ "res://m/d.txt" };
	BOOST_REQUIRE(layer.store(tag, test_util::to_bytes("x")));
	BOOST_REQUIRE(wait_until_there(root / "m" / "d.txt"));

	BOOST_TEST(!layer.erase(tag));
	BOOST_TEST(layer.exists(tag));
	BOOST_TEST(std::filesystem::exists(root / "m" / "d.txt"));
}

BOOST_AUTO_TEST_SUITE_END()
