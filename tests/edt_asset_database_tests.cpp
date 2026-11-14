#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <chrono>
#include <boost/json.hpp>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

res::file_change change(std::string_view path, res::change_kind kind, std::string_view old_path = {})
{
	res::file_change out;
	out.tag = res::tag::make(path);
	if (!old_path.empty()) out.old_tag = res::tag::make(old_path);
	out.kind = kind;
	return out;
}

// A change the editor made itself (resource_system store/create_sync).
res::file_change own_change(std::string_view path, res::change_kind kind)
{
	res::file_change out = change(path, kind);
	out.self = true;
	return out;
}

res::file_change folder_change(std::string_view path, res::change_kind kind)
{
	res::file_change out = change(path, kind);
	out.is_directory = true;
	return out;
}

constexpr std::string_view k_guid_a = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_guid_b = "00000000-0000-4000-8000-000000000002";
constexpr std::string_view k_guid_c = "00000000-0000-4000-8000-000000000003";
constexpr std::string_view k_guid_d = "00000000-0000-4000-8000-000000000004";

struct asset_db_fixture
{
	asset_db_fixture() : lower("asset_db_lower"), upper("asset_db_upper"), cache("asset_db_cache") {}

	std::unique_ptr<edt::asset_database> cached()
	{
		auto made = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() },
			edt::asset_cache_location{ cache.path(), "asset_db" });
		made->rebuild();
		test_util::pump(tasks);
		return made;
	}

	// Older than the stamp granularity: a cache trusts what it learned of the file.
	static void age(const fs::path& path)
	{
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	fs::path write_aged(std::string_view relative, std::string_view text)
	{
		const fs::path path = upper.write(relative, text);
		age(path);
		return path;
	}

	void write_cache_text(std::string_view text)
	{
		fs::create_directories(cache.path() / "asset_db");
		std::ofstream(cache.path() / "asset_db" / "assets.json", std::ios::binary | std::ios::trunc) << text;
	}

	// The cache's JSON after a change made by `edit`, written back: for tests of what loading refuses.
	template <typename F>
	void edit_cache(F&& edit)
	{
		boost::json::value document = boost::json::parse(cache_bytes());
		edit(document.as_object());
		write_cache_text(boost::json::serialize(document));
	}

	std::string cache_bytes() const
	{
		std::ifstream in(cache.path() / "asset_db" / "assets.json", std::ios::binary);
		return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
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
	test_util::temp_dir cache;
	// Runs queued work until the walk's result was applied, before any hashing ran.
	void run_walk()
	{
		while (db->scanning()) {
			BOOST_REQUIRE(tasks.run_one(ds::lane::io) || tasks.drain_main(std::chrono::hours(1)) > 0);
		}
	}

	std::unique_ptr<edt::asset_database> unopened()
	{
		return std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
	}

	// What a fresh database's full walk finds equals what `assets` holds (the times of the checks aside).
	void expect_same_as_a_full_walk(const edt::asset_database& assets)
	{
		auto fresh = unopened();
		fresh->rebuild();
		test_util::pump(tasks);
		BOOST_TEST(assets.records().size() == fresh->records().size());
		for (const auto& [key, expected] : fresh->records()) {
			BOOST_TEST_CONTEXT(key) {
				const edt::asset_record* found = assets.find(expected.path);
				BOOST_REQUIRE(found);
				BOOST_TEST(found->same_as(expected));
			}
		}
		// last_hash is what this database saw of the file before it went: a fresh walk cannot know it.
		const auto without_hash = [](std::vector<edt::asset_orphan> orphans) {
			for (edt::asset_orphan& orphan : orphans) {
				orphan.last_hash.clear();
			}
			return orphans;
		};
		BOOST_TEST((without_hash(assets.orphans()) == without_hash(fresh->orphans())));
		BOOST_TEST((assets.duplicates() == fresh->duplicates()));
	}

	std::optional<edt::job_service::job_id> job_named(std::string_view label) const
	{
		for (const auto& job : jobs.list()) {
			if (job.label == label) return job.id;
		}
		return std::nullopt;
	}

	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
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
	BOOST_TEST((stone.shadowed_layers == std::vector<std::size_t>{ 0u }));

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

// One rule with res::is_asset_meta_name (#817): a sidecar in any letter case is a sidecar, not an asset, and belongs to
// its file.
BOOST_AUTO_TEST_CASE(a_sidecar_in_any_letter_case_is_a_sidecar)
{
	upper.write("A.PNG", "a");
	upper.write("A.PNG.RESOURCE", meta_text(k_guid_a));
	upper.write("b.png", "b");
	upper.write("b.png.Resource", meta_text(k_guid_b));
	auto& assets = open();

	BOOST_TEST(assets.find(res::tag::make("A.PNG.RESOURCE")) == nullptr);
	BOOST_TEST(assets.find(res::tag::make("b.png.Resource")) == nullptr);
	BOOST_TEST(assets.records().size() == 2u);
	BOOST_REQUIRE(record("A.PNG").id.has_value());
	BOOST_TEST(res::to_text(*record("A.PNG").id) == k_guid_a);
	BOOST_REQUIRE(record("b.png").id.has_value());
	BOOST_TEST(res::to_text(*record("b.png").id) == k_guid_b);
	BOOST_TEST(assets.orphans().empty());
}

BOOST_AUTO_TEST_CASE(a_bad_sidecar_leaves_the_file_without_id_and_says_why)
{
	upper.write("broken.png", "x");
	upper.write("broken.png.resource", R"({ "__type": "resource_meta", "__version": 1 })");

	open();
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
	run_walk();
	// Nothing changed, so nothing is announced (and nothing written).
	BOOST_TEST(assets.revision() == revision);
	BOOST_TEST(record("kept.txt").content_hash == hash);
	BOOST_TEST(assets.pending_hashes() == 0u);
}

#if !defined(_WIN32)
// Windows junctions take the same path (a reparse point that redirects); CI cannot make one without rights, so only
// links are tested, on POSIX.
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
	// A lower layer whose folder is a link out of the tree, under the name of a real folder of the upper layer.
	const fs::path outside_dir = upper.path().parent_path() / "asset_db_outside_dir";
	fs::create_directories(outside_dir);
	{
		std::ofstream out(outside_dir / "foreign.png");
		out << "foreign";
	}
	fs::create_directory_symlink(outside_dir, lower.path() / "textures", ec);
	BOOST_REQUIRE(!ec);
	upper.write("textures/own.png", "own");

	auto& assets = open();
	BOOST_TEST(assets.find(res::tag::make("link.txt")) == nullptr);
	BOOST_TEST(assets.find(res::tag::make("real.txt")) != nullptr);
	BOOST_TEST(assets.find(res::tag::make("textures/own.png")) != nullptr);
	BOOST_TEST(assets.find(res::tag::make("textures/foreign.png")) == nullptr);
	fs::remove(outside, ec);
	fs::remove_all(outside_dir, ec);
}
#endif

// Review #816: a hashing job cancelled from outside (job.cancel) does not stop hashing for good.
BOOST_AUTO_TEST_CASE(a_cancelled_hashing_job_goes_on_at_the_next_rebuild)
{
	upper.write("a.txt", "a");
	upper.write("b.txt", "b");
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
	db->rebuild();
	run_walk();
	const auto hashing = job_named("Hash assets");
	BOOST_REQUIRE(hashing.has_value());
	BOOST_REQUIRE(jobs.cancel(*hashing));
	test_util::pump(tasks);
	BOOST_TEST(db->pending_hashes() == 2u);
	BOOST_TEST(record("a.txt").content_hash.empty());

	db->rebuild();
	test_util::pump(tasks);
	BOOST_TEST(db->pending_hashes() == 0u);
	BOOST_TEST(!record("a.txt").content_hash.empty());
	BOOST_TEST(!record("b.txt").content_hash.empty());
}

BOOST_AUTO_TEST_CASE(a_cancelled_walk_is_not_scanning_any_more)
{
	upper.write("a.txt", "a");
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
	db->rebuild();
	const auto walking = job_named("Scan assets");
	BOOST_REQUIRE(walking.has_value());
	BOOST_REQUIRE(jobs.cancel(*walking));
	test_util::pump(tasks);
	BOOST_TEST(!db->scanning());
	db->rebuild();
	test_util::pump(tasks);
	BOOST_TEST(db->find(res::tag::make("a.txt")) != nullptr);
}

// A file written after the walk saw it is not given the hash of its new bytes under its old size and time.
BOOST_AUTO_TEST_CASE(a_file_written_after_the_walk_is_not_hashed_under_its_old_stamp)
{
	upper.write("moving.txt", "first");
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
	db->rebuild();
	run_walk();
	upper.write("moving.txt", "second, longer");
	test_util::pump(tasks);
	BOOST_TEST(record("moving.txt").content_hash.empty());
	BOOST_TEST(record("moving.txt").size == 5);

	db->rebuild();
	test_util::pump(tasks);
	BOOST_TEST(record("moving.txt").size == 14);
	BOOST_TEST(!record("moving.txt").content_hash.empty());
}

BOOST_AUTO_TEST_CASE(hidden_folders_are_not_walked)
{
	upper.write(".git/config", "x");
	upper.write(".hidden/a.txt", "x");
	upper.write("shown/a.txt", "x");
	auto& assets = open();
	BOOST_TEST(assets.records().size() == 1u);
	BOOST_TEST(assets.find(res::tag::make("shown/a.txt")) != nullptr);
}

// AC #760: a database built from the cache reads no file that did not change.
BOOST_AUTO_TEST_CASE(the_cache_spares_reading_unchanged_files)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	write_aged("b.desc", R"({ "__type": "b_type" })");
	{
		auto first = cached();
		BOOST_TEST(first->writes_cache());
		BOOST_TEST(!first->loaded_from_cache());
		BOOST_TEST(first->files_read() == 3u); // a.png.resource, a.png, b.desc
	}
	BOOST_REQUIRE(!cache_bytes().empty());

	auto second = cached();
	BOOST_TEST(second->loaded_from_cache());
	BOOST_TEST(second->files_read() == 0u);
	const edt::asset_record* a = second->find(res::tag::make("a.png"));
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(a->id.has_value());
	BOOST_TEST(res::to_text(*a->id) == k_guid_a);
	BOOST_TEST(!a->content_hash.empty());
	const edt::asset_record* b = second->find(res::tag::make("b.desc"));
	BOOST_REQUIRE(b);
	BOOST_TEST((b->desc_type == std::optional<std::string>{ "b_type" }));
}

BOOST_AUTO_TEST_CASE(only_a_changed_file_is_read_again)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	write_aged("b.txt", "b");
	cached().reset();

	write_aged("b.txt", "b, longer now");
	auto second = cached();
	BOOST_TEST(second->files_read() == 1u);
	BOOST_TEST(second->find(res::tag::make("b.txt"))->size == 13);
}

// AC #760: a second editor on the project reads the cache and never writes it.
BOOST_AUTO_TEST_CASE(a_second_editor_does_not_write_the_cache)
{
	upper.write("a.txt", "a");
	auto first = cached();
	BOOST_REQUIRE(first->writes_cache());
	const std::string before = cache_bytes();
	BOOST_REQUIRE(!before.empty());

	upper.write("new.txt", "new");
	{
		auto second = cached();
		BOOST_TEST(!second->writes_cache());
		BOOST_TEST(second->loaded_from_cache());
		BOOST_TEST(second->find(res::tag::make("new.txt")) != nullptr);
		second->save();
	}
	BOOST_TEST(cache_bytes() == before);
}

BOOST_AUTO_TEST_CASE(a_cache_of_other_layers_is_not_used)
{
	upper.write("a.txt", "a");
	cached().reset();
	auto other = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ upper.path() },
		edt::asset_cache_location{ cache.path(), "asset_db" });
	BOOST_TEST(!other->loaded_from_cache());
}

// A batch finds a .resource in any letter case, as the walk does, and its going makes no orphan of another name.
BOOST_AUTO_TEST_CASE(a_batch_pairs_a_sidecar_in_any_letter_case)
{
	upper.write("a.png", "a");
	auto& assets = open();
	BOOST_TEST(!record("a.png").id.has_value());

	upper.write("a.png.RESOURCE", meta_text(k_guid_a));
	assets.apply_changes(std::vector<res::file_change>{ change("a.png.RESOURCE", res::change_kind::created) });
	BOOST_REQUIRE(record("a.png").id.has_value());
	BOOST_TEST(res::to_text(*record("a.png").id) == k_guid_a);
	BOOST_TEST(assets.find(res::tag::make("a.png.RESOURCE")) == nullptr);

	fs::remove(upper.path() / "a.png");
	assets.apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted) });
	BOOST_REQUIRE(assets.orphans().size() == 1u);
	BOOST_TEST(assets.orphans().front().meta_path.string() == "res://a.png.RESOURCE");

	fs::remove(upper.path() / "a.png.RESOURCE");
	assets.apply_changes(std::vector<res::file_change>{ change("a.png.RESOURCE", res::change_kind::deleted) });
	BOOST_TEST(assets.orphans().empty());
}

// AC #760: create, change and delete of a file and of its .resource are each seen in one batch.
BOOST_AUTO_TEST_CASE(a_batch_adds_changes_and_removes_files_and_sidecars)
{
	upper.write("kept.txt", "kept");
	auto& assets = open();

	upper.write("new.png", "new");
	upper.write("new.png.resource", meta_text(k_guid_a));
	upper.write("kept.txt", "kept, longer");
	const std::vector<res::file_change> first{ change("new.png", res::change_kind::created),
		change("new.png.resource", res::change_kind::created), change("kept.txt", res::change_kind::modified) };
	const edt::asset_batch added = assets.apply_changes(first);
	BOOST_REQUIRE(added.added.size() == 1u);
	BOOST_TEST(added.added.front().string() == "res://new.png");
	BOOST_REQUIRE(added.changed.size() == 1u);
	BOOST_TEST(added.changed.front().string() == "res://kept.txt");
	BOOST_REQUIRE(record("new.png").id.has_value());
	BOOST_TEST(res::to_text(*record("new.png").id) == k_guid_a);
	test_util::pump(tasks);
	BOOST_TEST(!record("new.png").content_hash.empty());
	BOOST_TEST(record("kept.txt").size == 12);

	fs::remove(upper.path() / "new.png");
	fs::remove(upper.path() / "new.png.resource");
	const std::vector<res::file_change> second{ change("new.png", res::change_kind::deleted),
		change("new.png.resource", res::change_kind::deleted) };
	const edt::asset_batch removed = assets.apply_changes(second);
	BOOST_REQUIRE(removed.removed.size() == 1u);
	BOOST_TEST(assets.find(res::tag::make("new.png")) == nullptr);
	BOOST_TEST(assets.find(*res::asset_id_from_text(k_guid_a)).empty());
	BOOST_TEST(assets.orphans().empty());
}

// AC #760: a file moved with its .resource, seen as deleted + created, is a move found by the guid.
BOOST_AUTO_TEST_CASE(a_move_is_matched_by_the_guid_of_the_sidecar)
{
	upper.write("old/rock.png", "rock");
	upper.write("old/rock.png.resource", meta_text(k_guid_a));
	auto& assets = open();
	const std::string hash = record("old/rock.png").content_hash;
	BOOST_REQUIRE(!hash.empty());

	fs::create_directories(upper.path() / "new");
	fs::rename(upper.path() / "old/rock.png", upper.path() / "new/stone.png");
	fs::rename(upper.path() / "old/rock.png.resource", upper.path() / "new/stone.png.resource");
	const std::vector<res::file_change> moved{ change("old/rock.png", res::change_kind::deleted),
		change("old/rock.png.resource", res::change_kind::deleted), change("new/stone.png", res::change_kind::created),
		change("new/stone.png.resource", res::change_kind::created) };
	const edt::asset_batch batch = assets.apply_changes(moved);
	BOOST_REQUIRE(batch.moved.size() == 1u);
	BOOST_TEST(batch.moved.front().first.string() == "res://old/rock.png");
	BOOST_TEST(batch.moved.front().second.string() == "res://new/stone.png");
	BOOST_TEST(batch.added.empty());
	BOOST_TEST(batch.removed.empty());
	BOOST_TEST(res::to_text(*record("new/stone.png").id) == k_guid_a);
	BOOST_TEST(record("new/stone.png").content_hash == hash);
	BOOST_TEST(assets.pending_hashes() == 0u);
}

BOOST_AUTO_TEST_CASE(a_rename_the_watcher_reported_is_a_move_too)
{
	upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_a));
	auto& assets = open();
	fs::rename(upper.path() / "a.png", upper.path() / "b.png");
	fs::rename(upper.path() / "a.png.resource", upper.path() / "b.png.resource");
	const std::vector<res::file_change> renamed{ change("b.png", res::change_kind::renamed, "a.png"),
		change("b.png.resource", res::change_kind::renamed, "a.png.resource") };
	const edt::asset_batch batch = assets.apply_changes(renamed);
	BOOST_REQUIRE(batch.moved.size() == 1u);
	BOOST_TEST(batch.moved.front().second.string() == "res://b.png");
}

// A file that moved without its .resource: an orphan where it was, a file without ID where it is.
BOOST_AUTO_TEST_CASE(a_file_moved_without_its_sidecar_leaves_an_orphan)
{
	upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_a));
	auto& assets = open();
	fs::rename(upper.path() / "a.png", upper.path() / "b.png");
	const std::vector<res::file_change> moved{ change("a.png", res::change_kind::deleted), change("b.png", res::change_kind::created) };
	const edt::asset_batch batch = assets.apply_changes(moved);
	BOOST_TEST(batch.moved.empty());
	BOOST_REQUIRE(batch.added.size() == 1u);
	BOOST_TEST(!record("b.png").id.has_value());
	BOOST_REQUIRE(assets.orphans().size() == 1u);
	BOOST_TEST(assets.orphans().front().meta_path.string() == "res://a.png.resource");
	BOOST_TEST(res::to_text(*assets.orphans().front().id) == k_guid_a);
}

// #822: a file stamped as close as the stamp granularity to the time it was checked could change again without its
// stamp moving; what was learned from it is read again after a restart.
BOOST_AUTO_TEST_CASE(a_stamp_close_to_its_check_is_not_trusted)
{
	upper.write("fresh.png", "fresh");
	upper.write("fresh.png.resource", meta_text(k_guid_a));
	write_aged("old.txt", "old");
	cached().reset();

	auto second = cached();
	BOOST_TEST(second->loaded_from_cache());
	BOOST_TEST(second->files_read() == 2u); // fresh.png.resource and fresh.png, not old.txt
	const edt::asset_record* fresh = second->find(res::tag::make("fresh.png"));
	BOOST_REQUIRE(fresh);
	BOOST_TEST(!fresh->content_hash.empty());
	BOOST_TEST(fresh->id.has_value());
}

// #822 A: the file's check and the .resource's are apart: a racy file next to a trusted .resource is hashed once,
// and the next start trusts the new hash.
BOOST_AUTO_TEST_CASE(a_racy_file_with_a_trusted_sidecar_is_hashed_once)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	cached().reset();
	// As if the file had been hashed in the tick of its last change; its .resource stays trusted.
	edit_cache([](boost::json::object& root) {
		boost::json::object& record = root.at("records").as_array().at(0).as_object();
		record["content_checked_at"] = record.at("mtime");
	});

	BOOST_TEST(cached()->files_read() == 1u);
	BOOST_TEST(cached()->files_read() == 0u);
}

// #822: nothing of the cache is published before the first walk confirmed it; a file gone meanwhile never shows.
BOOST_AUTO_TEST_CASE(the_cache_is_not_published_before_the_walk)
{
	write_aged("kept.txt", "kept");
	write_aged("gone.txt", "gone");
	cached().reset();
	fs::remove(upper.path() / "gone.txt");

	edt::asset_database second(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() },
		edt::asset_cache_location{ cache.path(), "asset_db" });
	BOOST_TEST(second.loaded_from_cache());
	BOOST_TEST(second.records().empty());
	BOOST_TEST(second.find(res::tag::make("kept.txt")) == nullptr);
	second.rebuild();
	test_util::pump(tasks);
	BOOST_TEST(second.find(res::tag::make("kept.txt")) != nullptr);
	BOOST_TEST(second.find(res::tag::make("gone.txt")) == nullptr);
	BOOST_TEST(second.files_read() == 0u);
}

// A .resource that changed while its file did not is read again; and the other way round only the file is hashed.
BOOST_AUTO_TEST_CASE(a_changed_sidecar_or_a_changed_file_is_read_alone)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	cached().reset();

	write_aged("a.png.resource", meta_text(k_guid_b) + "\n");
	{
		auto second = cached();
		BOOST_TEST(second->files_read() == 1u);
		BOOST_TEST(res::to_text(*second->find(res::tag::make("a.png"))->id) == k_guid_b);
	}

	write_aged("a.png", "a, longer");
	auto third = cached();
	BOOST_TEST(third->files_read() == 1u);
	BOOST_TEST(res::to_text(*third->find(res::tag::make("a.png"))->id) == k_guid_b);
}

// #822: a cache that is not what save() writes is dropped whole, and the walk reads every file.
BOOST_AUTO_TEST_CASE(a_bad_cache_is_dropped_whole)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	cached().reset();
	const std::string good = cache_bytes();
	BOOST_REQUIRE(!good.empty());

	using edit = std::function<void(boost::json::object&)>;
	const auto record_edit = [](std::function<void(boost::json::object&)> change) {
		return edit([change](boost::json::object& root) { change(root.at("records").as_array().at(0).as_object()); });
	};
	const std::vector<std::pair<std::string, edit>> cases = {
		{ "another __type", [](boost::json::object& root) { root["__type"] = "other"; } },
		{ "another __version", [](boost::json::object& root) { root["__version"] = 1; } },
		{ "the version before meta_layer", [](boost::json::object& root) { root["__version"] = 2; } },
		{ "records not an array", [](boost::json::object& root) { root["records"] = 5; } },
		{ "a record not an object", [](boost::json::object& root) { root.at("records").as_array().at(0) = 1; } },
		{ "a path without res://", record_edit([](boost::json::object& r) { r["path"] = "a.png"; }) },
		{ "a path with ..", record_edit([](boost::json::object& r) { r["path"] = "res://../a.png"; }) },
		{ "an absolute path", record_edit([](boost::json::object& r) { r["path"] = "res:///etc/a.png"; }) },
		{ "a type of another suffix", record_edit([](boost::json::object& r) { r["type"] = "desc"; }) },
		{ "a hash without its prefix", record_edit([](boost::json::object& r) { r["hash"] = "0123456789abcdef"; }) },
		{ "a layer out of range", record_edit([](boost::json::object& r) { r["layer"] = 7; }) },
		{ "a meta layer out of range", record_edit([](boost::json::object& r) { r["meta_layer"] = 7; }) },
		{ "a missing meta layer", record_edit([](boost::json::object& r) { r.erase("meta_layer"); }) },
		{ "a shadowed layer not below", record_edit([](boost::json::object& r) { r["shadowed"] = boost::json::array{ 1 }; }) },
		{ "a bad id", record_edit([](boost::json::object& r) { r["id"] = "nope"; }) },
		{ "a bad file id", record_edit([](boost::json::object& r) { r["file_id"] = boost::json::array{ 1, 2 }; }) },
		{ "a missing check time", record_edit([](boost::json::object& r) { r.erase("content_checked_at"); }) },
		{ "a path twice", [](boost::json::object& root) {
			boost::json::array& records = root.at("records").as_array();
			records.push_back(records.at(0));
		} },
	};
	for (const auto& [name, change] : cases) {
		BOOST_TEST_CONTEXT(name) {
			write_cache_text(good);
			edit_cache(change);
			auto again = cached();
			BOOST_TEST(!again->loaded_from_cache());
			BOOST_TEST(again->files_read() == 2u);
			BOOST_TEST(again->find(res::tag::make("a.png")) != nullptr);
		}
	}

	for (const char* text : { "{ not json", "[1, 2]" }) {
		BOOST_TEST_CONTEXT(text) {
			write_cache_text(text);
			auto again = cached();
			BOOST_TEST(!again->loaded_from_cache());
			BOOST_TEST(again->files_read() == 2u);
		}
	}
}

BOOST_AUTO_TEST_CASE(a_cache_over_the_limit_is_not_read)
{
	write_aged("a.txt", "a");
	write_cache_text(std::string(edt::asset_database::max_cache_bytes + 1, ' '));
	auto again = cached();
	BOOST_TEST(!again->loaded_from_cache());
	BOOST_TEST(again->find(res::tag::make("a.txt")) != nullptr);
}

// #822: nothing changed since the cache was written, so a walk writes nothing.
BOOST_AUTO_TEST_CASE(an_unchanged_walk_does_not_write_the_cache)
{
	write_aged("a.txt", "a");
	cached().reset();
	const fs::path file = cache.path() / "asset_db" / "assets.json";
	const auto written = fs::last_write_time(file);
	fs::last_write_time(file, written - std::chrono::hours(2));
	const auto aged = fs::last_write_time(file);

	auto again = cached();
	again->rebuild();
	test_util::pump(tasks);
	BOOST_TEST((fs::last_write_time(file) == aged));
}

#if !defined(_WIN32)
// #822: a project could hold links where the cache goes. Nothing outside it is read, written or locked, and the
// database is built from the files.
BOOST_AUTO_TEST_CASE(links_at_the_cache_are_not_followed)
{
	test_util::temp_dir outside("asset_db_outside");
	const fs::path victim = outside.path() / "victim";
	std::ofstream(victim, std::ios::binary) << "keep";
	write_aged("a.txt", "a");

	const auto victim_text = [&victim] {
		std::ifstream in(victim, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), {});
	};

	// The cache file: not read and not replaced.
	fs::create_directories(cache.path() / "asset_db");
	fs::create_symlink(victim, cache.path() / "asset_db" / "assets.json");
	{
		auto db_link = cached();
		BOOST_TEST(!db_link->loaded_from_cache());
		BOOST_TEST(db_link->find(res::tag::make("a.txt")) != nullptr);
	}
	BOOST_TEST(victim_text() == "keep");
	fs::remove_all(cache.path() / "asset_db");

	// The lock file: not opened, so this editor neither locks nor writes.
	fs::create_directories(cache.path() / "asset_db");
	fs::create_symlink(victim, cache.path() / "asset_db" / ".lock");
	{
		auto lock_link = cached();
		BOOST_TEST(!lock_link->writes_cache());
	}
	BOOST_TEST(victim_text() == "keep");
	BOOST_TEST(!fs::exists(cache.path() / "asset_db" / "assets.json"));
	fs::remove_all(cache.path() / "asset_db");

	// The folder: nothing is made in the folder it leads to.
	fs::create_directory_symlink(outside.path(), cache.path() / "asset_db");
	{
		auto folder_link = cached();
		BOOST_TEST(!folder_link->writes_cache());
		BOOST_TEST(folder_link->find(res::tag::make("a.txt")) != nullptr);
	}
	BOOST_TEST(!fs::exists(outside.path() / "assets.json"));
	BOOST_TEST(!fs::exists(outside.path() / ".lock"));
}
#endif

// #822: on Windows the walk asks with the \\?\ prefix, which takes '\' only; layers named with '/' still walk.
BOOST_AUTO_TEST_CASE(layers_named_with_forward_slashes_are_walked)
{
	upper.write("sub/a.txt", "a");
	const fs::path generic{ upper.path().generic_string() };
	const edt::asset_scan scan = edt::scan_asset_layers({ generic });
	BOOST_REQUIRE_EQUAL(scan.records.size(), 1u);
	BOOST_TEST(scan.records[0].path.string() == "res://sub/a.txt");
}

BOOST_AUTO_TEST_CASE(the_kind_of_asset_comes_from_the_suffix)
{
	BOOST_TEST(edt::asset_type_of("a/b.DESC") == "desc");
	BOOST_TEST(edt::asset_type_of("t.Png") == "texture");
	BOOST_TEST(edt::asset_type_of("m.gltf") == "model");
	BOOST_TEST(edt::asset_type_of("s.frag") == "shader");
	BOOST_TEST(edt::asset_type_of("readme.md") == "file");
}

// Review #829: a batch that comes while a walk runs is applied after the walk's result, which would replace it.
BOOST_AUTO_TEST_CASE(a_batch_during_a_walk_is_applied_after_it)
{
	upper.write("kept.txt", "kept");
	upper.write("doomed.txt", "doomed");
	db = unopened();
	std::vector<edt::asset_batch> seen;
	db->set_on_batch([&seen](const edt::asset_batch& batch) { seen.push_back(batch); });
	db->rebuild();
	// The walk ran and listed the folder; its result waits on the main queue.
	BOOST_REQUIRE(tasks.run_one(ds::lane::io));
	BOOST_REQUIRE(db->scanning());

	upper.write("born.txt", "born");
	fs::remove(upper.path() / "doomed.txt");
	const edt::asset_batch waiting = db->apply_changes(std::vector<res::file_change>{ change("born.txt", res::change_kind::created),
		change("doomed.txt", res::change_kind::deleted) });
	BOOST_TEST(waiting.added.empty());
	BOOST_TEST(seen.empty());

	test_util::pump(tasks);
	BOOST_TEST(!db->scanning());
	BOOST_TEST(db->find(res::tag::make("born.txt")) != nullptr);
	BOOST_TEST(db->find(res::tag::make("doomed.txt")) == nullptr);
	BOOST_TEST(db->find(res::tag::make("kept.txt")) != nullptr);
	BOOST_REQUIRE(seen.size() == 1u);
	BOOST_REQUIRE(seen.front().added.size() == 1u);
	BOOST_TEST(seen.front().added.front().string() == "res://born.txt");
	BOOST_REQUIRE(seen.front().removed.size() == 1u);
	BOOST_TEST(seen.front().removed.front().string() == "res://doomed.txt");
}

// Review #829: a change the walk saw before its batch was replayed is still reported, by the walk's own batch.
BOOST_AUTO_TEST_CASE(a_change_the_walk_saw_first_is_reported)
{
	upper.write("kept.txt", "kept");
	auto& assets = open();
	std::vector<edt::asset_batch> seen;
	assets.set_on_batch([&seen](const edt::asset_batch& batch) { seen.push_back(batch); });

	upper.write("kept.txt", "kept, longer");
	upper.write("new.txt", "new");
	assets.rebuild();
	BOOST_REQUIRE(tasks.run_one(ds::lane::io));
	assets.apply_changes(std::vector<res::file_change>{ change("kept.txt", res::change_kind::modified),
		change("new.txt", res::change_kind::created) });
	test_util::pump(tasks);

	const auto reported = [&seen](const auto& list_of, std::string_view path) {
		return std::ranges::any_of(seen, [&](const edt::asset_batch& batch) {
			return std::ranges::any_of(list_of(batch), [path](const res::tag& tag) { return tag.view() == "res://" + std::string{ path }; });
		});
	};
	BOOST_TEST(reported([](const edt::asset_batch& b) -> const std::vector<res::tag>& { return b.changed; }, "kept.txt"));
	BOOST_TEST(reported([](const edt::asset_batch& b) -> const std::vector<res::tag>& { return b.added; }, "new.txt"));
}

// Review #829: a cancelled first walk is started again once, and the batch that waited for it is applied.
BOOST_AUTO_TEST_CASE(a_cancelled_first_walk_is_started_again)
{
	upper.write("a.txt", "a");
	db = unopened();
	int walks = 0;
	const auto counting = events.job_started.subscribe([&walks](std::uint64_t, const std::string& label) {
		if (label == "Scan assets") {
			++walks;
		}
	});
	db->rebuild();
	upper.write("x.txt", "x");
	db->apply_changes(std::vector<res::file_change>{ change("x.txt", res::change_kind::created) });
	const auto walking = job_named("Scan assets");
	BOOST_REQUIRE(walking.has_value());
	BOOST_REQUIRE(jobs.cancel(*walking));
	test_util::pump(tasks);
	BOOST_TEST(walks == 2);
	BOOST_TEST(!db->scanning());
	BOOST_TEST(db->find(res::tag::make("x.txt")) != nullptr);
	BOOST_TEST(db->find(res::tag::make("a.txt")) != nullptr);
}

// Review #829: a batch at startup, before the first walk was applied, does not make the walk forget the cache.
BOOST_AUTO_TEST_CASE(an_early_batch_keeps_the_cached_hashes)
{
	write_aged("a.txt", "a");
	write_aged("b.desc", R"({ "__type": "b_type" })");
	cached().reset();

	edt::asset_database second(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() },
		edt::asset_cache_location{ cache.path(), "asset_db" });
	second.rebuild();
	write_aged("c.txt", "c");
	second.apply_changes(std::vector<res::file_change>{ change("c.txt", res::change_kind::created) });
	BOOST_TEST(second.records().empty());
	test_util::pump(tasks);
	BOOST_TEST(second.files_read() == 1u); // c.txt only
	BOOST_REQUIRE(second.records().size() == 3u);
	BOOST_TEST(!second.find(res::tag::make("c.txt"))->content_hash.empty());
	BOOST_TEST((second.find(res::tag::make("b.desc"))->desc_type == std::optional<std::string>{ "b_type" }));
}

// Review #829: Windows rules (tested on any system): a rename in letter case is one file, not two with one guid.
BOOST_AUTO_TEST_CASE(a_rename_in_letter_case_keeps_one_record)
{
	upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_a));
	db = unopened();
	// The injected mode folds names only; folders spelled differently across layers on a case-sensitive disk are not
	// covered (a limitation of testing the Windows rules on Linux).
	db->set_case_insensitive(true);
	db->rebuild();
	test_util::pump(tasks);
	BOOST_REQUIRE(db->find(res::tag::make("a.png")) != nullptr);

	fs::rename(upper.path() / "a.png", upper.path() / "A.png");
	db->apply_changes(std::vector<res::file_change>{ change("A.png", res::change_kind::renamed, "a.png") });
	BOOST_TEST(db->records().size() == 1u);
	BOOST_TEST(db->find(res::tag::make("a.png")) == nullptr);
	BOOST_REQUIRE(record("A.png").id.has_value());
	BOOST_TEST(res::to_text(*record("A.png").id) == k_guid_a);
	BOOST_TEST(db->duplicates().empty());
	BOOST_TEST(db->orphans().empty());

	// Reported as a deleted and a created, in either order.
	fs::rename(upper.path() / "A.png", upper.path() / "a.png");
	db->apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::created), change("A.png", res::change_kind::deleted) });
	BOOST_TEST(db->records().size() == 1u);
	BOOST_TEST(db->find(res::tag::make("a.png")) != nullptr);
	BOOST_TEST(db->duplicates().empty());
	expect_same_as_a_full_walk(*db);
}

// Review #829: case insensitive, a .resource in another letter case than its file belongs to it, as in the walk; an edit
// of it makes no record of the name it is spelled with.
BOOST_AUTO_TEST_CASE(a_sidecar_spelled_in_another_case_makes_no_phantom_record)
{
	upper.write("b.png", "b");
	upper.write("B.png.resource", meta_text(k_guid_b));
	db = unopened();
	db->set_case_insensitive(true);
	db->rebuild();
	test_util::pump(tasks);
	BOOST_REQUIRE(record("b.png").id.has_value());
	BOOST_TEST(res::to_text(*record("b.png").id) == k_guid_b);

	upper.write("B.png.resource", meta_text(k_guid_a) + "\n");
	db->apply_changes(std::vector<res::file_change>{ change("B.png.resource", res::change_kind::modified) });
	BOOST_TEST(db->records().size() == 1u);
	BOOST_TEST(db->find(res::tag::make("B.png")) == nullptr);
	BOOST_REQUIRE(record("b.png").id.has_value());
	BOOST_TEST(res::to_text(*record("b.png").id) == k_guid_a);
	BOOST_TEST(db->orphans().empty());
}

// Review #829: a file changed while its hashing job read it is hashed again with its new bytes.
BOOST_AUTO_TEST_CASE(a_file_changed_while_its_hash_is_read_is_hashed_again)
{
	upper.write("a.txt", "first");
	db = unopened();
	db->rebuild();
	run_walk();
	// The hashing job read "first"; its result waits on the main queue.
	BOOST_REQUIRE(tasks.run_one(ds::lane::io));
	upper.write("a.txt", "second, longer");
	db->apply_changes(std::vector<res::file_change>{ change("a.txt", res::change_kind::modified) });
	test_util::pump(tasks);

	const edt::asset_content now = edt::read_asset_content(upper.path(), "a.txt", false, edt::asset_database::max_desc_bytes);
	BOOST_REQUIRE(now.ok);
	BOOST_TEST(record("a.txt").size == 14);
	BOOST_TEST(record("a.txt").content_hash == now.content_hash);
	BOOST_TEST(db->pending_hashes() == 0u);
}

// Review #829: folder events during a walk do not restart it; one more walk follows it.
BOOST_AUTO_TEST_CASE(folder_events_during_a_walk_make_one_more_walk)
{
	upper.write("a.txt", "a");
	db = unopened();
	int walks = 0;
	const auto counting = events.job_started.subscribe([&walks](std::uint64_t, const std::string& label) {
		if (label == "Scan assets") {
			++walks;
		}
	});
	db->rebuild();
	for (int i = 0; i < 3; ++i) {
		db->apply_changes(std::vector<res::file_change>{ folder_change("sub", res::change_kind::created) });
	}
	BOOST_TEST(walks == 1);
	test_util::pump(tasks);
	BOOST_TEST(!db->scanning());
	BOOST_TEST(walks == 2);
}

BOOST_AUTO_TEST_CASE(a_folder_event_walks_again)
{
	upper.write("a.txt", "a");
	auto& assets = open();
	upper.write("new_dir/x.txt", "x");
	assets.apply_changes(std::vector<res::file_change>{ folder_change("new_dir", res::change_kind::created) });
	BOOST_TEST(assets.scanning());
	test_util::pump(tasks);
	BOOST_TEST(assets.find(res::tag::make("new_dir/x.txt")) != nullptr);
}

// Review #829: what the walk skips (hidden folders) is not walked again or looked up for a batch.
BOOST_AUTO_TEST_CASE(changes_below_hidden_folders_are_ignored)
{
	upper.write("a.txt", "a");
	auto& assets = open();
	const std::uint64_t revision = assets.revision();
	upper.write(".git/index", "x");
	upper.write(".hidden/x.txt", "x");
	upper.write("shown/.cache/y.txt", "y");
	assets.apply_changes(std::vector<res::file_change>{ folder_change(".git", res::change_kind::created),
		change(".git/index", res::change_kind::modified), folder_change(".hidden/sub", res::change_kind::created),
		change(".hidden/x.txt", res::change_kind::created), change("shown/.cache/y.txt", res::change_kind::created) });
	BOOST_TEST(!assets.scanning());
	BOOST_TEST(assets.revision() == revision);
	BOOST_TEST(assets.records().size() == 1u);
}

// A file of the top layer covers the one below while it is there; the lower one shows again when it goes.
BOOST_AUTO_TEST_CASE(a_top_file_shadows_the_lower_one_while_it_is_there)
{
	lower.write("s.txt", "lower");
	auto& assets = open();
	BOOST_TEST(record("s.txt").layer == 0u);

	upper.write("s.txt", "upper!");
	assets.apply_changes(std::vector<res::file_change>{ change("s.txt", res::change_kind::created) });
	BOOST_TEST(record("s.txt").layer == 1u);
	BOOST_TEST((record("s.txt").shadowed_layers == std::vector<std::size_t>{ 0u }));
	BOOST_TEST(record("s.txt").size == 6);

	fs::remove(upper.path() / "s.txt");
	assets.apply_changes(std::vector<res::file_change>{ change("s.txt", res::change_kind::deleted) });
	BOOST_TEST(batch.removed.empty());
	BOOST_REQUIRE(batch.changed.size() == 1u);
	BOOST_TEST(record("s.txt").layer == 0u);
	BOOST_TEST(record("s.txt").shadowed_layers.empty());
	BOOST_TEST(record("s.txt").size == 5);
}

// The walk lists nothing below max_depth folders; a batch makes no record there either.
BOOST_AUTO_TEST_CASE(a_file_deeper_than_the_walk_goes_is_not_a_record)
{
	std::string deep;
	for (int i = 0; i < edt::asset_database::max_depth; ++i) {
		deep += "d/";
	}
	const std::string shallow = deep.substr(2) + "f.txt";
	deep += "f.txt";
	upper.write(shallow, "ok");
	auto& assets = open();
	BOOST_TEST(assets.find(res::tag::make(shallow)) != nullptr);

	upper.write(deep, "deep");
	assets.apply_changes(std::vector<res::file_change>{ change(deep, res::change_kind::created) });
	BOOST_TEST(assets.find(res::tag::make(deep)) == nullptr);
	expect_same_as_a_full_walk(assets);
}

#if !defined(_WIN32)
// Review #829: a link in the top layer (a file, or a folder on the way) is skipped as the walk skips it: the lower
// layer's file shows.
BOOST_AUTO_TEST_CASE(a_link_in_the_top_layer_lets_the_lower_file_show)
{
	test_util::temp_dir outside("asset_db_outside");
	const fs::path foreign = outside.write("tex/a.png", "foreign");
	lower.write("tex/a.png", "lower");
	lower.write("s.txt", "lower");
	std::error_code ec;
	fs::create_directory_symlink(outside.path() / "tex", upper.path() / "tex", ec);
	BOOST_REQUIRE(!ec);
	fs::create_symlink(foreign, upper.path() / "s.txt", ec);
	BOOST_REQUIRE(!ec);
	auto& assets = open();
	BOOST_TEST(record("tex/a.png").layer == 0u);
	BOOST_TEST(record("s.txt").layer == 0u);

	lower.write("tex/a.png", "lower, changed");
	lower.write("s.txt", "lower, changed");
	assets.apply_changes(std::vector<res::file_change>{ change("tex/a.png", res::change_kind::modified),
		change("s.txt", res::change_kind::modified) });
	BOOST_TEST(record("tex/a.png").layer == 0u);
	BOOST_TEST(record("tex/a.png").shadowed_layers.empty());
	BOOST_TEST(record("tex/a.png").size == 14);
	BOOST_TEST(record("s.txt").layer == 0u);
	BOOST_TEST(record("s.txt").shadowed_layers.empty());
	expect_same_as_a_full_walk(assets);
}
#endif

// Review #829: batches of creates, edits, renames, deletes and moves leave what a full walk of the same layers finds.
BOOST_AUTO_TEST_CASE(incremental_batches_match_a_full_walk)
{
	lower.write("s.txt", "lower s");
	lower.write("dir/l.desc", R"({ "__type": "low" })");
	upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_a));
	upper.write("b.txt", "b");
	upper.write("dir/c.desc", R"({ "__type": "c" })");
	upper.write("dir/c.desc.resource", meta_text(k_guid_b));
	auto& assets = open();

	upper.write("n.png", "n");
	upper.write("n.png.resource", meta_text(k_guid_c));
	upper.write("b.txt", "b, longer");
	assets.apply_changes(std::vector<res::file_change>{ change("n.png", res::change_kind::created),
		change("n.png.resource", res::change_kind::created), change("b.txt", res::change_kind::modified) });

	fs::rename(upper.path() / "a.png", upper.path() / "dir/a2.png");
	fs::rename(upper.path() / "a.png.resource", upper.path() / "dir/a2.png.resource");
	upper.write("s.txt", "upper s, covers");
	const edt::asset_batch moved = assets.apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted),
		change("a.png.resource", res::change_kind::deleted), change("dir/a2.png", res::change_kind::created),
		change("dir/a2.png.resource", res::change_kind::created), change("s.txt", res::change_kind::created) });
	BOOST_TEST(moved.moved.size() == 1u);

	fs::remove(upper.path() / "dir/c.desc");
	upper.write("b.txt.resource", meta_text(k_guid_d));
	fs::remove(upper.path() / "n.png.resource");
	upper.write("ghost.png.resource", meta_text(k_guid_c));
	assets.apply_changes(std::vector<res::file_change>{ change("dir/c.desc", res::change_kind::deleted),
		change("b.txt.resource", res::change_kind::created), change("n.png.resource", res::change_kind::deleted),
		change("ghost.png.resource", res::change_kind::created) });

	fs::rename(upper.path() / "b.txt", upper.path() / "b2.txt");
	fs::rename(upper.path() / "b.txt.resource", upper.path() / "b2.txt.resource");
	fs::remove(upper.path() / "s.txt");
	assets.apply_changes(std::vector<res::file_change>{ change("b2.txt", res::change_kind::renamed, "b.txt"),
		change("b2.txt.resource", res::change_kind::renamed, "b.txt.resource"), change("s.txt", res::change_kind::deleted) });
	test_util::pump(tasks);

	BOOST_TEST(record("s.txt").layer == 0u);
	BOOST_TEST(assets.orphans().size() == 2u); // dir/c.desc.resource, ghost.png.resource
	expect_same_as_a_full_walk(assets);
}

// The database follows res:// of a resource_system it is attached to.
BOOST_AUTO_TEST_CASE(an_attached_database_follows_the_watcher)
{
	test_util::temp_dir project("asset_db_project");
	const fs::path content = project.path() / "res";
	const fs::path cache_dir = project.path() / ".cache";
	fs::create_directories(content);
	fs::create_directories(cache_dir / "memory");
	project.write("res/a.txt", "a");
	res::resource_system resources(tasks, res::resource_system::project_mount_table({}, { content }, cache_dir, cache_dir / "user_profile"));

	edt::asset_database assets(jobs, &events, resources.get_resource_layers());
	assets.attach(resources);
	assets.rebuild();
	test_util::pump(tasks);
	BOOST_REQUIRE(assets.find(res::tag::make("a.txt")) != nullptr);

	// Written beside and moved in: the watcher never sees it half written.
	fs::rename(project.write("staging.tmp", "b"), content / "b.txt");
	fs::remove(content / "a.txt");
	const auto seen = [&assets] {
		return assets.find(res::tag::make("b.txt")) != nullptr && assets.find(res::tag::make("a.txt")) == nullptr;
	};
	// The watcher polls on its own thread and resource_system has no call that polls now: one generous wait.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!seen() && std::chrono::steady_clock::now() < deadline) {
		test_util::pump(tasks);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	BOOST_TEST(seen());
}

// #854: in the layer that is the project folder (it holds project.snkproj) the project's code, build output and project file
// are no assets and shadow nothing below; the walk and a batch agree.
BOOST_AUTO_TEST_CASE(the_project_folder_layer_hides_its_code_build_and_project_file)
{
	upper.write("project.snkproj", "{}");
	upper.write("project.snkproj.resource", meta_text(k_guid_a));
	upper.write("code/main.cpp", "x");
	upper.write("build/out.bin", "x");
	upper.write("textures/a.png", "x");
	lower.write("code/lib.txt", "x");
	auto& assets = open();
	BOOST_TEST(assets.find(res::tag::make("textures/a.png")) != nullptr);
	BOOST_TEST(assets.find(res::tag::make("code/lib.txt")) != nullptr);
	BOOST_TEST(assets.find(res::tag::make("code/main.cpp")) == nullptr);
	BOOST_TEST(assets.find(res::tag::make("build/out.bin")) == nullptr);
	BOOST_TEST(assets.find(res::tag::make("project.snkproj")) == nullptr);
	BOOST_TEST(assets.orphans().empty());

	upper.write("code/other.cpp", "x");
	const std::vector<res::file_change> batch{ change("code/other.cpp", res::change_kind::created) };
	BOOST_TEST(assets.apply_changes(batch).added.empty());
	expect_same_as_a_full_walk(assets);
}

// #854: a layer without project.snkproj is no project folder: its code/ and build/ hold assets like any folder.
BOOST_AUTO_TEST_CASE(a_layer_without_a_project_file_lists_its_code_and_build_folders)
{
	upper.write("code/main.cpp", "x");
	upper.write("build/out.bin", "x");
	auto& assets = open();
	BOOST_TEST(assets.find(res::tag::make("code/main.cpp")) != nullptr);
	BOOST_TEST(assets.find(res::tag::make("build/out.bin")) != nullptr);
}

// #888: two files and their .resource written with one size and one time (as in one clock tick); one pair is moved over
// the other. The path takes the moved file's ID and hash: the stamps tell the files apart by their identity.
static void write_same_stamp_pair(test_util::temp_dir& dir)
{
	dir.write("a.png", "aa");
	dir.write("a.png.resource", meta_text(k_guid_a));
	dir.write("b.png", "bb");
	dir.write("b.png.resource", meta_text(k_guid_b));
	const auto when = fs::file_time_type::clock::now() - std::chrono::hours(1);
	for (const char* name : { "a.png", "a.png.resource", "b.png", "b.png.resource" }) {
		fs::last_write_time(dir.path() / name, when);
	}
}

static void move_b_over_a(test_util::temp_dir& dir)
{
	fs::rename(dir.path() / "b.png", dir.path() / "a.png");
	fs::rename(dir.path() / "b.png.resource", dir.path() / "a.png.resource");
}

// What the watcher reports of move_b_over_a.
static std::vector<res::file_change> b_moved_over_a()
{
	return { change("a.png", res::change_kind::renamed, "b.png"),
		change("a.png.resource", res::change_kind::renamed, "b.png.resource") };
}

BOOST_AUTO_TEST_CASE(a_file_moved_over_one_of_the_same_size_and_time_brings_its_id_and_hash)
{
	write_same_stamp_pair(upper);
	auto& assets = open();
	const std::string hash_of_b = record("b.png").content_hash;
	BOOST_REQUIRE(!hash_of_b.empty());
	BOOST_REQUIRE(record("a.png").content_hash != hash_of_b);
	BOOST_REQUIRE(record("a.png").size == record("b.png").size);
	BOOST_REQUIRE(record("a.png").meta_size == record("b.png").meta_size);

	move_b_over_a(upper);
	assets.apply_changes(b_moved_over_a());
	test_util::pump(tasks);
	BOOST_REQUIRE(record("a.png").id.has_value());
	BOOST_TEST(res::to_text(*record("a.png").id) == k_guid_b);
	BOOST_TEST(record("a.png").content_hash == hash_of_b);
	BOOST_TEST(assets.find(res::tag::make("b.png")) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_walk_over_the_cache_tells_a_file_moved_over_another_of_the_same_stamp)
{
	write_same_stamp_pair(upper);
	std::string hash_of_b;
	{
		auto first = cached();
		hash_of_b = first->find(res::tag::make("b.png"))->content_hash;
	}
	BOOST_REQUIRE(!hash_of_b.empty());

	move_b_over_a(upper);
	auto second = cached();
	BOOST_TEST(second->loaded_from_cache());
	const edt::asset_record* a = second->find(res::tag::make("a.png"));
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(a->id.has_value());
	BOOST_TEST(res::to_text(*a->id) == k_guid_b);
	BOOST_TEST(a->content_hash == hash_of_b);
	BOOST_TEST(second->files_read() == 2u); // a.png.resource and a.png, now b's
}

// #888: a file system that tells no identity (some network and synchronised ones): the .resource is read again every
// time, so the moved one still gives its ID; hashes follow size and time alone, and that is warned of once per layer.
BOOST_AUTO_TEST_CASE(without_file_identities_a_resource_is_read_again_and_the_layer_is_warned_of_once)
{
	write_same_stamp_pair(upper);
	std::size_t warnings = 0;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view category, engine::log_level level,
		std::string_view message) {
		if (category == "assets" && level == engine::log_level::warn
			&& message.find("tells no file identity") != std::string_view::npos) {
			++warnings;
		}
	});
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
	db->set_file_id_source([](const fs::path&) { return edt::file_id{}; });
	db->rebuild();
	test_util::pump(tasks);
	BOOST_TEST(warnings == 1u);

	move_b_over_a(upper);
	db->apply_changes(b_moved_over_a());
	BOOST_REQUIRE(record("a.png").id.has_value());
	BOOST_TEST(res::to_text(*record("a.png").id) == k_guid_b);
	BOOST_TEST(warnings == 1u);
}

// #888: a cache written before the stamps had identities is taken, and what it learned is read again once.
BOOST_AUTO_TEST_CASE(a_cache_without_file_identities_is_read_again_once)
{
	write_aged("a.png", "a");
	write_aged("a.png.resource", meta_text(k_guid_a));
	cached().reset();
	BOOST_REQUIRE(cache_bytes().find("\"file_id\"") != std::string::npos);
	edit_cache([](boost::json::object& root) {
		for (boost::json::value& record : root.at("records").as_array()) {
			record.as_object().erase("file_id");
			record.as_object().erase("meta_file_id");
		}
	});

	auto second = cached();
	BOOST_TEST(second->loaded_from_cache());
	BOOST_TEST(second->files_read() == 2u);
	BOOST_TEST(res::to_text(*second->find(res::tag::make("a.png"))->id) == k_guid_a);
	second->save();
	second.reset();
	auto third = cached();
	BOOST_TEST(third->files_read() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// #765: the layers in a folder of their own, with a git folder the tests hand to the database when they need one.
struct new_id_fixture
{
	new_id_fixture() : repo("asset_db_repo")
	{
		fs::create_directories(git());
		fs::create_directories(lower());
		fs::create_directories(upper());
	}

	fs::path git() const { return repo.path() / ".git"; }
	fs::path lower() const { return repo.path() / "engine"; }
	fs::path upper() const { return repo.path() / "project" / "res"; }

	static fs::path write(const fs::path& layer, std::string_view relative, std::string_view text)
	{
		const fs::path path = layer / relative;
		fs::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
		return path;
	}

	// Older than new_file_quiet_time: a file from outside that is done being written.
	static fs::path write_aged(const fs::path& layer, std::string_view relative, std::string_view text)
	{
		const fs::path path = write(layer, relative, text);
		age(path);
		return path;
	}

	static std::string read(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	static void age(const fs::path& path)
	{
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower(), upper() });
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	void apply(res::file_change change)
	{
		db->apply_changes(std::vector<res::file_change>{ std::move(change) });
	}

	// Pumps the work and looks at the waiting files once more.
	void settle()
	{
		test_util::pump(tasks);
		db->poll(true);
	}

	// The ID in <upper>/<relative>.resource; none when there is no readable one.
	std::optional<res::asset_id> meta_id(std::string_view relative) const
	{
		const fs::path path = upper() / (std::string{ relative } + ".resource");
		if (!fs::is_regular_file(path)) {
			return std::nullopt;
		}
		const res::asset_meta_result parsed = res::read_asset_meta(read(path));
		if (!parsed) {
			return std::nullopt;
		}
		return parsed.meta->guid;
	}

	const edt::asset_record& record(std::string_view path) const
	{
		const edt::asset_record* found = db->find(res::tag::make(path));
		BOOST_REQUIRE_MESSAGE(found, std::string{ path });
		return *found;
	}

	static std::string desc_with_guid(std::string_view guid)
	{
		return std::string{ R"({ "__type": "level_desc", "__guid": ")" } + std::string{ guid } + R"(" })";
	}

	test_util::temp_dir repo;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetNewIds, new_id_fixture)

BOOST_AUTO_TEST_CASE(an_own_new_file_gets_an_id_at_once_and_keeps_it)
{
	write(upper(), "kept.txt", "kept");
	open();
	write(upper(), "tex/new.png", "new");
	apply(own_change("tex/new.png", res::change_kind::created));

	const std::optional<res::asset_id> id = meta_id("tex/new.png");
	BOOST_REQUIRE(id.has_value());
	BOOST_TEST(!id->is_nil());
	BOOST_TEST((record("tex/new.png").id == id));
	BOOST_TEST(record("tex/new.png").has_meta);
	const std::string sidecar = read(upper() / "tex/new.png.resource");

	write(upper(), "tex/new.png", "new, written again");
	apply(own_change("tex/new.png", res::change_kind::modified));
	settle();
	BOOST_TEST((record("tex/new.png").id == id));
	BOOST_TEST(read(upper() / "tex/new.png.resource") == sidecar);
	BOOST_TEST(!fs::exists(upper() / "kept.txt.resource"));
}

BOOST_AUTO_TEST_CASE(a_file_from_before_the_first_walk_gets_no_id)
{
	write_aged(upper(), "old.png", "old");
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower(), upper() });
	db->rebuild();
	// Reported before the first walk was applied: the walk finds it, so it is left to migration.
	write_aged(upper(), "early.png", "early");
	apply(own_change("early.png", res::change_kind::created));
	test_util::pump(tasks);
	BOOST_REQUIRE(db->find(res::tag::make("early.png")) != nullptr);

	write_aged(upper(), "old.png", "old, written again");
	apply(own_change("old.png", res::change_kind::modified));
	apply(change("old.png", res::change_kind::modified));
	settle();
	BOOST_TEST(!fs::exists(upper() / "old.png.resource"));
	BOOST_TEST(!fs::exists(upper() / "early.png.resource"));
	BOOST_TEST(!record("old.png").id.has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(a_new_desc_takes_the_guid_of_its_root)
{
	open();
	write(upper(), "levels/a.desc", desc_with_guid(k_guid_b));
	apply(own_change("levels/a.desc", res::change_kind::created));
	// The root is read by the hashing pass, off the main thread.
	BOOST_TEST(!meta_id("levels/a.desc").has_value());
	settle();
	BOOST_REQUIRE(meta_id("levels/a.desc").has_value());
	BOOST_TEST(res::to_text(*meta_id("levels/a.desc")) == k_guid_b);
	BOOST_TEST((record("levels/a.desc").id == res::asset_id_from_text(k_guid_b)));

	write(upper(), "levels/bad.desc", R"({ "__type": "level_desc", "__guid": "not an id" })");
	apply(own_change("levels/bad.desc", res::change_kind::created));
	write(upper(), "levels/nil.desc", R"({ "__guid": "00000000-0000-0000-0000-000000000000" })");
	apply(own_change("levels/nil.desc", res::change_kind::created));
	write(upper(), "levels/nested.desc", std::string{ R"({ "inner": { "__guid": ")" } + std::string{ k_guid_c } + R"(" } })");
	apply(own_change("levels/nested.desc", res::change_kind::created));
	settle();
	for (const std::string_view path : { "levels/bad.desc", "levels/nil.desc", "levels/nested.desc" }) {
		BOOST_TEST_CONTEXT(path) {
			const std::optional<res::asset_id> id = meta_id(path);
			BOOST_REQUIRE(id.has_value());
			BOOST_TEST(!id->is_nil());
			BOOST_TEST((id != res::asset_id_from_text(k_guid_b)));
			BOOST_TEST((id != res::asset_id_from_text(k_guid_c)));
		}
	}
}

BOOST_AUTO_TEST_CASE(a_copied_desc_does_not_take_the_guid_in_use)
{
	open();
	write(upper(), "levels/a.desc", desc_with_guid(k_guid_b));
	apply(own_change("levels/a.desc", res::change_kind::created));
	settle();
	BOOST_REQUIRE((meta_id("levels/a.desc") == res::asset_id_from_text(k_guid_b)));

	write(upper(), "levels/copy.desc", desc_with_guid(k_guid_b));
	apply(own_change("levels/copy.desc", res::change_kind::created));
	settle();
	const std::optional<res::asset_id> copy = meta_id("levels/copy.desc");
	BOOST_REQUIRE(copy.has_value());
	BOOST_TEST((copy != res::asset_id_from_text(k_guid_b)));
	BOOST_TEST(db->duplicates().empty());
}

BOOST_AUTO_TEST_CASE(a_lower_file_or_one_with_a_resource_gets_nothing)
{
	open();
	write(lower(), "below.png", "below");
	apply(own_change("below.png", res::change_kind::created));
	BOOST_TEST(!fs::exists(lower() / "below.png.resource"));
	BOOST_TEST(!fs::exists(upper() / "below.png.resource"));
	BOOST_TEST(!record("below.png").id.has_value());

	write(upper(), "has.png", "has");
	write(upper(), "has.png.resource", meta_text(k_guid_a));
	apply(own_change("has.png", res::change_kind::created));
	apply(own_change("has.png.resource", res::change_kind::created));
	BOOST_TEST(read(upper() / "has.png.resource") == meta_text(k_guid_a));
	BOOST_TEST((record("has.png").id == res::asset_id_from_text(k_guid_a)));

	// A .resource only in a lower layer is the file's .resource too.
	write(lower(), "shared.png.resource", meta_text(k_guid_b));
	write(upper(), "shared.png", "shared");
	apply(own_change("shared.png", res::change_kind::created));
	BOOST_TEST(!fs::exists(upper() / "shared.png.resource"));
	BOOST_TEST((record("shared.png").id == res::asset_id_from_text(k_guid_b)));
}

BOOST_AUTO_TEST_CASE(an_outside_new_file_gets_an_id_after_its_hash)
{
	open();
	write_aged(upper(), "ext.png", "external");
	apply(change("ext.png", res::change_kind::created));
	BOOST_TEST(!fs::exists(upper() / "ext.png.resource"));
	BOOST_TEST(!record("ext.png").id.has_value());
	BOOST_TEST(db->pending_ids() == 1u);

	test_util::pump(tasks);
	const std::optional<res::asset_id> id = meta_id("ext.png");
	BOOST_REQUIRE(id.has_value());
	BOOST_TEST((record("ext.png").id == id));
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(an_outside_file_waits_until_it_is_quiet)
{
	open();
	write(upper(), "fresh.png", "fresh");
	apply(change("fresh.png", res::change_kind::created));
	settle();
	BOOST_TEST(!fs::exists(upper() / "fresh.png.resource"));
	BOOST_TEST(db->pending_ids() == 1u);

	age(upper() / "fresh.png");
	apply(change("fresh.png", res::change_kind::modified));
	settle();
	BOOST_TEST(meta_id("fresh.png").has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

// res::is_ignored_asset_name: one rule for the walk, the batches and the new IDs.
BOOST_AUTO_TEST_CASE(ignored_names_are_no_records_and_get_no_id)
{
	const std::vector<std::string_view> ignored{ ".gitkeep", ".DS_Store", "Thumbs.db", "~$x.docx", "a.tmp", "b~" };
	write_aged(upper(), "before/.gitkeep", "");
	write_aged(upper(), "before/Thumbs.db", "thumbs");
	write_aged(upper(), "before/a.tmp.resource", meta_text(k_guid_a));
	open();
	BOOST_TEST(db->records().empty());
	BOOST_TEST(db->orphans().empty());

	for (const std::string_view name : ignored) {
		write_aged(upper(), name, "junk");
		apply(own_change(name, res::change_kind::created));
		apply(change(name, res::change_kind::modified));
	}
	write_aged(upper(), "a.tmp.png", "an asset");
	write_aged(upper(), "x.tmpl", "an asset too");
	apply(own_change("a.tmp.png", res::change_kind::created));
	apply(change("x.tmpl", res::change_kind::created));
	settle();
	for (const std::string_view name : ignored) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(db->find(res::tag::make(name)) == nullptr);
			BOOST_TEST(!fs::exists(upper() / (std::string{ name } + ".resource")));
		}
	}
	BOOST_TEST(meta_id("a.tmp.png").has_value());
	BOOST_TEST(meta_id("x.tmpl").has_value());
	BOOST_TEST(db->records().size() == 2u);
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(an_outside_file_with_the_content_of_an_orphan_is_left_for_link)
{
	write(upper(), "a.png", "same bytes");
	write(upper(), "a.png.resource", meta_text(k_guid_a));
	open();
	fs::remove(upper() / "a.png");
	apply(change("a.png", res::change_kind::deleted));
	write_aged(upper(), "b.png", "same bytes");
	apply(change("b.png", res::change_kind::created));
	settle();
	BOOST_TEST(!fs::exists(upper() / "b.png.resource"));
	BOOST_TEST(!record("b.png").id.has_value());
	BOOST_REQUIRE(db->orphans().size() == 1u);
	BOOST_TEST(db->orphans().front().last_hash == record("b.png").content_hash);
	BOOST_TEST(db->pending_ids() == 0u);
}

// An orphan whose content is unknown, or an empty file, says nothing of a move.
BOOST_AUTO_TEST_CASE(an_orphan_without_hash_or_an_empty_file_does_not_block)
{
	write(upper(), "lost.png.resource", meta_text(k_guid_a));
	write(upper(), "empty.png", "");
	write(upper(), "empty.png.resource", meta_text(k_guid_b));
	open();
	BOOST_REQUIRE(db->orphans().size() == 1u);
	BOOST_REQUIRE(db->orphans().front().last_hash.empty());
	fs::remove(upper() / "empty.png");
	apply(change("empty.png", res::change_kind::deleted));
	BOOST_REQUIRE(db->orphans().size() == 2u);

	write_aged(upper(), "new.png", "new");
	write_aged(upper(), "also_empty.png", "");
	db->apply_changes(std::vector<res::file_change>{ change("new.png", res::change_kind::created),
		change("also_empty.png", res::change_kind::created) });
	settle();
	BOOST_TEST(meta_id("new.png").has_value());
	BOOST_TEST(meta_id("also_empty.png").has_value());
}

BOOST_AUTO_TEST_CASE(a_resource_that_appears_while_waiting_is_kept)
{
	open();
	write_aged(upper(), "ext.png", "external");
	apply(change("ext.png", res::change_kind::created));
	// Written beside it with no event yet.
	write(upper(), "ext.png.resource", meta_text(k_guid_a));
	settle();
	BOOST_TEST(read(upper() / "ext.png.resource") == meta_text(k_guid_a));
	BOOST_TEST((record("ext.png").id == res::asset_id_from_text(k_guid_a)));
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(an_own_git_operation_holds_the_id_back)
{
	bool busy = true;
	open().set_vcs_busy([&busy] { return busy; });
	write_aged(upper(), "pulled.png", "pulled");
	apply(change("pulled.png", res::change_kind::created));
	settle();
	BOOST_TEST(!fs::exists(upper() / "pulled.png.resource"));
	BOOST_TEST(db->pending_ids() == 1u);

	busy = false;
	db->poll(true);
	BOOST_TEST(meta_id("pulled.png").has_value());
	BOOST_TEST(record("pulled.png").id.has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(a_git_lock_or_a_fresh_index_holds_the_id_back)
{
	open().set_git_dir_provider([this]() -> std::optional<fs::path> { return git(); });
	write(git(), "index.lock", "");
	write_aged(upper(), "checked_out.png", "checked out");
	apply(change("checked_out.png", res::change_kind::created));
	settle();
	BOOST_TEST(!fs::exists(upper() / "checked_out.png.resource"));

	fs::remove(git() / "index.lock");
	write(git(), "index", "index");
	write(git(), "HEAD", "ref: refs/heads/main\n");
	db->poll(true);
	BOOST_TEST(!fs::exists(upper() / "checked_out.png.resource"));
	BOOST_TEST(db->pending_ids() == 1u);

	age(git() / "index");
	age(git() / "HEAD");
	db->poll(true);
	BOOST_TEST(meta_id("checked_out.png").has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

// git init during the session: the git folder is asked for at every check.
BOOST_AUTO_TEST_CASE(a_git_folder_that_comes_later_is_used)
{
	std::optional<fs::path> provided;
	open().set_git_dir_provider([&provided] { return provided; });
	write(git(), "index.lock", "");
	write_aged(upper(), "first.png", "first");
	apply(change("first.png", res::change_kind::created));
	settle();
	BOOST_TEST(meta_id("first.png").has_value());

	provided = git();
	write_aged(upper(), "second.png", "second");
	apply(change("second.png", res::change_kind::created));
	settle();
	BOOST_TEST(!meta_id("second.png").has_value());

	fs::remove(git() / "index.lock");
	db->poll(true);
	BOOST_TEST(meta_id("second.png").has_value());
}

BOOST_AUTO_TEST_CASE(a_renamed_file_gets_no_id)
{
	write(upper(), "a.png", "a");
	open();
	fs::rename(upper() / "a.png", upper() / "b.png");
	apply(change("b.png", res::change_kind::renamed, "a.png"));
	settle();
	BOOST_TEST(!fs::exists(upper() / "b.png.resource"));
	BOOST_TEST(!record("b.png").id.has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

// A move seen as a deleted and a created in two polls, in either order.
BOOST_AUTO_TEST_CASE(a_move_split_over_two_polls_gets_no_id)
{
	write_aged(upper(), "a.txt", "a");
	write_aged(upper(), "c.txt", "c, longer");
	open();
	fs::rename(upper() / "a.txt", upper() / "b.txt");
	apply(change("a.txt", res::change_kind::deleted));
	apply(change("b.txt", res::change_kind::created));
	settle();
	BOOST_TEST(!fs::exists(upper() / "b.txt.resource"));

	fs::rename(upper() / "c.txt", upper() / "d.txt");
	apply(change("d.txt", res::change_kind::created));
	apply(change("c.txt", res::change_kind::deleted));
	settle();
	BOOST_TEST(!fs::exists(upper() / "d.txt.resource"));
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(a_renamed_folder_gives_its_files_no_ids)
{
	write_aged(upper(), "old/x.png", "x");
	write_aged(upper(), "old/y.txt", "y, longer");
	open();
	fs::rename(upper() / "old", upper() / "new");
	db->apply_changes(std::vector<res::file_change>{ folder_change("old", res::change_kind::deleted),
		folder_change("new", res::change_kind::created) });
	settle();
	BOOST_REQUIRE(db->find(res::tag::make("new/x.png")) != nullptr);
	BOOST_TEST(!fs::exists(upper() / "new/x.png.resource"));
	BOOST_TEST(!fs::exists(upper() / "new/y.txt.resource"));
	BOOST_TEST(db->pending_ids() == 0u);
}

BOOST_AUTO_TEST_CASE(files_of_a_new_folder_get_ids_after_the_walk)
{
	write(upper(), "a.png", "same bytes");
	write(upper(), "a.png.resource", meta_text(k_guid_a));
	open();
	fs::remove(upper() / "a.png");
	apply(change("a.png", res::change_kind::deleted));
	BOOST_REQUIRE(db->orphans().size() == 1u);

	write_aged(upper(), "drop/one.png", "one");
	write_aged(upper(), "drop/two.txt", "two");
	write_aged(upper(), "drop/moved.png", "same bytes");
	apply(folder_change("drop", res::change_kind::created));
	BOOST_TEST(db->scanning());
	settle();
	BOOST_TEST(meta_id("drop/one.png").has_value());
	BOOST_TEST(meta_id("drop/two.txt").has_value());
	BOOST_TEST(record("drop/one.png").id.has_value());
	BOOST_TEST(record("drop/two.txt").id.has_value());
	BOOST_TEST(!fs::exists(upper() / "drop/moved.png.resource"));
	BOOST_TEST(!record("drop/moved.png").id.has_value());
	BOOST_TEST(db->pending_ids() == 0u);
}

// A file whose hash never comes stops waiting after pending_timeout; a gone file stops passing for a move after
// move_window.
BOOST_AUTO_TEST_CASE(waiting_ends_and_moves_are_forgotten_with_time)
{
	auto now = std::chrono::steady_clock::now();
	open().set_clock_for_tests([&now] { return now; });
	write_aged(upper(), "stuck.png", "stuck");
	apply(change("stuck.png", res::change_kind::created));
	// The hashing job is never run.
	BOOST_TEST(db->pending_ids() == 1u);
	db->poll();
	BOOST_TEST(db->pending_ids() == 1u);
	now += edt::asset_database::pending_timeout + edt::asset_database::poll_interval + std::chrono::seconds(1);
	db->poll();
	BOOST_TEST(db->pending_ids() == 0u);
	test_util::pump(tasks);
	BOOST_TEST(!fs::exists(upper() / "stuck.png.resource"));

	write_aged(upper(), "a.txt", "moved bytes");
	apply(own_change("a.txt", res::change_kind::modified));
	test_util::pump(tasks);
	fs::rename(upper() / "a.txt", upper() / "b.txt");
	apply(change("a.txt", res::change_kind::deleted));
	now += edt::asset_database::move_window + std::chrono::seconds(1);
	apply(change("b.txt", res::change_kind::created));
	settle();
	BOOST_TEST(meta_id("b.txt").has_value());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(files_under_a_linked_folder_get_no_id)
{
	test_util::temp_dir outside("asset_db_repo_outside");
	write_aged(outside.path(), "f.png", "foreign");
	open();
	std::error_code ec;
	fs::create_directory_symlink(outside.path(), upper() / "linked", ec);
	BOOST_REQUIRE(!ec);
	db->apply_changes(std::vector<res::file_change>{ folder_change("linked", res::change_kind::created),
		change("linked/f.png", res::change_kind::created) });
	settle();
	BOOST_TEST(db->find(res::tag::make("linked/f.png")) == nullptr);
	BOOST_TEST(!fs::exists(outside.path() / "f.png.resource"));
	BOOST_TEST(db->pending_ids() == 0u);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
