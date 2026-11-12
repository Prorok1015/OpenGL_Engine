#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	meta.flags = { "editor_only" };
	return res::write_asset_meta(meta);
}

constexpr std::string_view k_guid_a = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_guid_b = "00000000-0000-4000-8000-000000000002";

struct asset_db_fixture
{
	asset_db_fixture() : lower("asset_db_lower"), upper("asset_db_upper") {}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, std::vector<fs::path>{ lower.path(), upper.path() });
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	const edt::asset_record& record(std::string_view path) const
	{
		const edt::asset_record* found = db->find(res::tag::make(path));
		BOOST_REQUIRE_MESSAGE(found, std::string{ path });
		return *found;
	}

	test_util::temp_dir lower;
	test_util::temp_dir upper;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::job_service jobs{ tasks };
	std::unique_ptr<edt::asset_database> db;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetDatabase, asset_db_fixture)

BOOST_AUTO_TEST_CASE(the_walk_merges_the_layers_and_reads_the_sidecars)
{
	lower.write("textures/stone.png", "lower stone");
	lower.write("textures/stone.png.resource", meta_text(k_guid_b));
	lower.write("only_below.txt", "below");
	upper.write("textures/stone.png", "upper stone");
	upper.write("textures/stone.png.resource", meta_text(k_guid_a));
	upper.write("models/crate.glb", "glb");

	auto& assets = open();
	BOOST_TEST(!assets.scanning());
	BOOST_TEST(assets.records().size() == 3u);

	const auto& stone = record("textures/stone.png");
	BOOST_TEST(stone.layer == 1u);
	BOOST_REQUIRE(stone.id.has_value());
	BOOST_TEST(res::to_text(*stone.id) == k_guid_a);
	BOOST_TEST(stone.type == "texture");
	BOOST_TEST(stone.has_meta);
	BOOST_TEST((stone.flags == std::vector<std::string>{ "editor_only" }));
	BOOST_TEST(stone.size == 11);

	const auto& below = record("only_below.txt");
	BOOST_TEST(below.layer == 0u);
	BOOST_TEST(!below.id.has_value());
	BOOST_TEST(!below.has_meta);
	BOOST_TEST(below.type == "file");
	BOOST_TEST(record("models/crate.glb").type == "model");

	BOOST_TEST(assets.find(res::tag::make("textures/stone.png.resource")) == nullptr);
	BOOST_TEST(assets.find(*res::asset_id_from_text(k_guid_a)).size() == 1u);
}

BOOST_AUTO_TEST_CASE(hashes_and_desc_types_arrive_from_the_background)
{
	upper.write("levels/main.desc", R"({ "__type": "level_desc", "__version": 2 })");
	upper.write("data/plain.desc", R"({ "speed": 2 })");
	upper.write("a.txt", "same");
	upper.write("b.txt", "same");
	upper.write("c.txt", "other");

	auto& assets = open();
	BOOST_TEST(assets.pending_hashes() == 0u);

	BOOST_TEST(record("levels/main.desc").type == "desc");
	BOOST_TEST((record("levels/main.desc").desc_type == std::optional<std::string>{ "level_desc" }));
	BOOST_TEST(!record("data/plain.desc").desc_type.has_value());
	BOOST_TEST(!record("a.txt").desc_type.has_value());

	BOOST_TEST(record("a.txt").content_hash.starts_with("fnv1a64:"));
	BOOST_TEST(record("a.txt").content_hash.size() == std::string_view{ "fnv1a64:" }.size() + 16u);
	BOOST_TEST(record("a.txt").content_hash == record("b.txt").content_hash);
	BOOST_TEST(record("a.txt").content_hash != record("c.txt").content_hash);
}

BOOST_AUTO_TEST_CASE(a_new_type_in_the_file_is_seen_by_the_next_walk)
{
	upper.write("x.desc", R"({ "__type": "first" })");
	auto& assets = open();
	BOOST_TEST((record("x.desc").desc_type == std::optional<std::string>{ "first" }));
	const std::string first_hash = record("x.desc").content_hash;

	upper.write("x.desc", R"({ "__type": "second_type" })");
	assets.rebuild();
	test_util::pump(tasks);
	BOOST_TEST((record("x.desc").desc_type == std::optional<std::string>{ "second_type" }));
	BOOST_TEST(record("x.desc").content_hash != first_hash);
}

BOOST_AUTO_TEST_CASE(duplicate_ids_and_orphans_are_found)
{
	upper.write("one.png", "1");
	upper.write("one.png.resource", meta_text(k_guid_a));
	upper.write("copy_of_one.png", "1");
	upper.write("copy_of_one.png.resource", meta_text(k_guid_a));
	upper.write("gone.png.resource", meta_text(k_guid_b));

	auto& assets = open();
	const std::vector<res::asset_id> duplicates = assets.duplicates();
	BOOST_REQUIRE(duplicates.size() == 1u);
	BOOST_TEST(res::to_text(duplicates.front()) == k_guid_a);
	BOOST_TEST(assets.find(duplicates.front()).size() == 2u);

	BOOST_REQUIRE(assets.orphans().size() == 1u);
	BOOST_TEST(assets.orphans().front().meta_path.string() == "res://gone.png.resource");
	BOOST_REQUIRE(assets.orphans().front().id.has_value());
	BOOST_TEST(res::to_text(*assets.orphans().front().id) == k_guid_b);
}

BOOST_AUTO_TEST_CASE(a_bad_sidecar_leaves_the_file_without_id_and_says_why)
{
	upper.write("broken.png", "x");
	upper.write("broken.png.resource", R"({ "__type": "resource_meta", "__version": 1 })");

	auto& assets = open();
	const auto& broken = record("broken.png");
	BOOST_TEST(broken.has_meta);
	BOOST_TEST(!broken.id.has_value());
	BOOST_TEST(broken.meta_error.find("__guid") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_file_unchanged_since_the_last_walk_keeps_its_hash)
{
	upper.write("kept.txt", "kept");
	auto& assets = open();
	const std::string hash = record("kept.txt").content_hash;
	BOOST_REQUIRE(!hash.empty());
	const std::uint64_t revision = assets.revision();

	assets.rebuild();
	// The walk's result is applied before any hashing: the kept file already has its hash.
	while (assets.scanning()) {
		BOOST_REQUIRE(tasks.run_one(ds::lane::io) || tasks.drain_main(std::chrono::hours(1)) > 0);
	}
	BOOST_TEST(assets.revision() > revision);
	BOOST_TEST(record("kept.txt").content_hash == hash);
	BOOST_TEST(assets.pending_hashes() == 0u);
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(links_are_not_followed)
{
	const fs::path outside = upper.path().parent_path() / "asset_db_outside.txt";
	{
		std::ofstream out(outside);
		out << "secret";
	}
	std::error_code ec;
	fs::create_symlink(outside, upper.path() / "link.txt", ec);
	BOOST_REQUIRE(!ec);
	upper.write("real.txt", "real");

	auto& assets = open();
	BOOST_TEST(assets.find(res::tag::make("link.txt")) == nullptr);
	BOOST_TEST(assets.find(res::tag::make("real.txt")) != nullptr);
	fs::remove(outside, ec);
}
#endif

BOOST_AUTO_TEST_CASE(the_kind_of_asset_comes_from_the_suffix)
{
	BOOST_TEST(edt::asset_type_of("a/b.DESC") == "desc");
	BOOST_TEST(edt::asset_type_of("t.Png") == "texture");
	BOOST_TEST(edt::asset_type_of("m.gltf") == "model");
	BOOST_TEST(edt::asset_type_of("s.frag") == "shader");
	BOOST_TEST(edt::asset_type_of("readme.md") == "file");
}

BOOST_AUTO_TEST_SUITE_END()
