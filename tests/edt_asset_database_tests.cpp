#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <chrono>
#include <boost/json.hpp>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <iterator>
#include <optional>
#include <string>
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

constexpr std::string_view k_guid_a = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_guid_b = "00000000-0000-4000-8000-000000000002";

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
		{ "records not an array", [](boost::json::object& root) { root["records"] = 5; } },
		{ "a record not an object", [](boost::json::object& root) { root.at("records").as_array().at(0) = 1; } },
		{ "a path without res://", record_edit([](boost::json::object& r) { r["path"] = "a.png"; }) },
		{ "a path with ..", record_edit([](boost::json::object& r) { r["path"] = "res://../a.png"; }) },
		{ "an absolute path", record_edit([](boost::json::object& r) { r["path"] = "res:///etc/a.png"; }) },
		{ "a type of another suffix", record_edit([](boost::json::object& r) { r["type"] = "desc"; }) },
		{ "a hash without its prefix", record_edit([](boost::json::object& r) { r["hash"] = "0123456789abcdef"; }) },
		{ "a layer out of range", record_edit([](boost::json::object& r) { r["layer"] = 7; }) },
		{ "a shadowed layer not below", record_edit([](boost::json::object& r) { r["shadowed"] = boost::json::array{ 1 }; }) },
		{ "a bad id", record_edit([](boost::json::object& r) { r["id"] = "nope"; }) },
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

BOOST_AUTO_TEST_SUITE_END()
