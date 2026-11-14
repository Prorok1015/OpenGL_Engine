#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The asset database as resource_system's asset index (S57-7, #770 part 2).

namespace fs = std::filesystem;

namespace {

constexpr std::string_view k_guid_a = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	return res::write_asset_meta(meta);
}

res::asset_id id_a()
{
	return *res::asset_id_from_text(k_guid_a);
}

constexpr std::string_view k_guid_b = "8d1e4b2a-6c3f-4a9e-b7d0-2c4e6f8a0b1d";
constexpr std::string_view k_guid_c = "5b7c9d1e-2f3a-4b5c-8d6e-7f8091a2b3c4";

res::file_change change(std::string_view path, res::change_kind kind, std::string_view old_path = {})
{
	res::file_change out;
	out.tag = res::tag::make(path);
	if (!old_path.empty()) {
		out.old_tag = res::tag::make(old_path);
	}
	out.kind = kind;
	return out;
}

// A project: res/ (the one content layer) and .cache/, with resource_system over them and the database following it.
struct project_fixture
{
	project_fixture()
	{
		fs::create_directories(content);
		fs::create_directories(cache_dir / "memory");
	}

	// `engine`: a lower layer under res/ (empty: none).
	void make_system(const fs::path& engine = {})
	{
		resources = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table(engine, { content }, cache_dir, cache_dir / "user_profile"));
		assets = std::make_unique<edt::asset_database>(jobs, &events, resources->get_resource_layers());
		assets->attach(*resources);
	}

	void pump() { test_util::pump(tasks); }

	// What the database reports for `changes` (the watcher's thread is not waited for: its later batch of the same changes
	// finds them applied).
	edt::asset_batch apply(std::vector<res::file_change> changes) { return assets->apply_changes(changes); }

	// `snapshot` answers as one taken now from the whole database: for every file and every ID it holds, the paths of
	// `gone` and the IDs of `ids` (gone ones too).
	void expect_as_fresh(const edt::asset_index_snapshot& snapshot, const std::vector<res::tag>& gone,
		const std::vector<res::asset_id>& ids)
	{
		edt::asset_index_snapshot fresh;
		fresh.refresh(*assets);
		std::vector<res::tag> paths = gone;
		std::vector<res::asset_id> all_ids = ids;
		for (const auto& [key, record] : assets->records()) {
			paths.push_back(record.path);
			if (record.id && !record.id->is_nil()) {
				all_ids.push_back(*record.id);
			}
		}
		for (const res::tag& path : paths) {
			BOOST_TEST((snapshot.id_of(path) == fresh.id_of(path)), path.view());
		}
		for (const res::asset_id& id : all_ids) {
			BOOST_TEST((snapshot.path_of(id) == fresh.path_of(id)), res::to_text(id));
		}
	}

	// A new .resource for `relative` under res/, its old one aged first: a rewrite within one tick of the clock keeps the
	// size and time, and the database would take the file as unchanged.
	void rewrite_meta(std::string_view relative, std::string_view guid)
	{
		const fs::path meta = content / (std::string{ relative } + ".resource");
		fs::last_write_time(meta, fs::file_time_type::clock::now() - std::chrono::hours(1));
		project.write("res/" + std::string{ relative } + ".resource", meta_text(guid));
	}

	// The watcher polls on its own thread: one generous wait for what it reports.
	template <class Pred>
	bool wait_for(Pred&& seen)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!seen() && std::chrono::steady_clock::now() < deadline) {
			pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return seen();
	}

	test_util::temp_dir project{ "asset_index_project" };
	fs::path content = project.path() / "res";
	fs::path cache_dir = project.path() / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<res::resource_system> resources;
	std::unique_ptr<edt::asset_database> assets;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetIndexTests, project_fixture)

BOOST_AUTO_TEST_CASE(the_snapshot_answers_for_the_file_problems_names_the_original)
{
	// The older file sorts after the copy by path: the age decides (same layer), as in Problems.
	project.write("res/z_original.txt", "original");
	project.write("res/z_original.txt.resource", meta_text(k_guid_a));
	project.write("res/a_copy.txt", "copy");
	project.write("res/a_copy.txt.resource", meta_text(k_guid_a));
	project.write("res/plain.txt", "p");
	fs::last_write_time(content / "z_original.txt", fs::file_time_type::clock::now() - std::chrono::hours(2));
	fs::last_write_time(content / "a_copy.txt", fs::file_time_type::clock::now() - std::chrono::hours(1));
	make_system();
	assets->rebuild();
	pump();
	std::size_t warnings = 0;
	const engine::log_sink_handle sink = engine::add_log_sink([&warnings](std::string_view category, engine::log_level level,
		std::string_view message) {
		if (category == "assets" && level == engine::log_level::warn && message.find("a_copy.txt") != std::string_view::npos) {
			++warnings;
		}
	});

	edt::asset_index_snapshot snapshot;
	BOOST_TEST(snapshot.refresh(*assets));
	BOOST_TEST((snapshot.id_of(res::tag{ "res://z_original.txt" }) == std::optional{ id_a() }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://a_copy.txt" }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://plain.txt" }));
	BOOST_TEST((snapshot.path_of(id_a()) == std::optional{ res::tag{ "res://z_original.txt" } }));
	const edt::asset_record* original = assets->find(res::tag{ "res://z_original.txt" });
	const edt::asset_record* copy = assets->find(res::tag{ "res://a_copy.txt" });
	BOOST_REQUIRE(original);
	BOOST_REQUIRE(copy);
	BOOST_TEST(edt::holds_id_before(*original, *copy));
	BOOST_TEST(warnings == 1u);
	// Nothing changed: the same answers, and the copy is not warned about again.
	BOOST_TEST(!snapshot.refresh(*assets));
	BOOST_TEST(warnings == 1u);
}

BOOST_AUTO_TEST_CASE(the_lower_layer_keeps_an_id_even_when_its_file_is_newer)
{
	// An engine asset and a project copy of it under another name: the engine's file is what others refer to.
	project.write("engine/e.txt", "engine");
	project.write("engine/e.txt.resource", meta_text(k_guid_a));
	project.write("res/p.txt", "project");
	project.write("res/p.txt.resource", meta_text(k_guid_a));
	fs::last_write_time(content / "p.txt", fs::file_time_type::clock::now() - std::chrono::hours(2));
	fs::last_write_time(project.path() / "engine" / "e.txt", fs::file_time_type::clock::now() - std::chrono::hours(1));
	make_system(project.path() / "engine");
	assets->rebuild();
	pump();

	edt::asset_index_snapshot snapshot;
	snapshot.refresh(*assets);
	BOOST_TEST((snapshot.path_of(id_a()) == std::optional{ res::tag{ "res://e.txt" } }));
	BOOST_TEST((snapshot.id_of(res::tag{ "res://e.txt" }) == std::optional{ id_a() }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://p.txt" }));
}

BOOST_AUTO_TEST_CASE(the_snapshot_takes_spellings_as_the_database_merges_them)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	assets->set_case_insensitive(true);
	assets->rebuild();
	pump();

	edt::asset_index_snapshot snapshot;
	snapshot.refresh(*assets);
	BOOST_TEST((snapshot.id_of(res::tag{ "res://A.TXT" }) == std::optional{ id_a() }));
}

// #854: what the walk leaves out of the project folder layer (code/, build/, hidden folders) has no ID in the index, after
// a walk and after a batch.
BOOST_AUTO_TEST_CASE(the_snapshot_leaves_out_what_the_project_folder_layer_hides)
{
	constexpr std::string_view k_guid_code = "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6";
	constexpr std::string_view k_guid_build = "1c7d2e3f-4051-4b62-9c73-8d94ea5fb607";
	constexpr std::string_view k_guid_hidden = "2d8e3f40-5162-4c73-8d84-9ea5fb60c718";
	constexpr std::string_view k_guid_later = "3e9f4051-6273-4d84-9e95-afb60c71d829";
	project.write("res/project.snkproj", "{}");
	project.write("res/code/a.txt", "x");
	project.write("res/code/a.txt.resource", meta_text(k_guid_code));
	project.write("res/build/b.txt", "x");
	project.write("res/build/b.txt.resource", meta_text(k_guid_build));
	project.write("res/.hidden/c.txt", "x");
	project.write("res/.hidden/c.txt.resource", meta_text(k_guid_hidden));
	project.write("res/textures/t.txt", "x");
	project.write("res/textures/t.txt.resource", meta_text(k_guid_a));
	make_system();
	assets->rebuild();
	pump();

	edt::asset_index_snapshot snapshot;
	snapshot.refresh(*assets);
	BOOST_TEST((snapshot.id_of(res::tag{ "res://textures/t.txt" }) == std::optional{ id_a() }));
	for (const std::string_view hidden : { "res://code/a.txt", "res://build/b.txt", "res://.hidden/c.txt" }) {
		BOOST_TEST_INFO("tag: " << hidden);
		BOOST_TEST(!snapshot.id_of(res::tag{ hidden }));
	}
	for (const std::string_view guid : { k_guid_code, k_guid_build, k_guid_hidden }) {
		BOOST_TEST_INFO("id: " << guid);
		BOOST_TEST(!snapshot.path_of(*res::asset_id_from_text(guid)));
	}

	project.write("res/code/later.txt", "x");
	project.write("res/code/later.txt.resource", meta_text(k_guid_later));
	res::file_change created;
	created.tag = res::tag{ "res://code/later.txt" };
	created.kind = res::change_kind::created;
	assets->apply_changes(std::span{ &created, 1 });
	snapshot.refresh(*assets);
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://code/later.txt" }));
	BOOST_TEST(!snapshot.path_of(*res::asset_id_from_text(k_guid_later)));
}

BOOST_AUTO_TEST_CASE(applied_through_counts_a_batch_held_behind_a_walk_once_it_is_applied)
{
	make_system();
	assets->rebuild();
	pump();
	BOOST_REQUIRE(resources->store_sync(res::tag{ "res://x.txt" }, std::vector<std::byte>{ std::byte{ 'x' } }));
	pump();
	const std::uint64_t first = resources->delivering_batch();
	BOOST_TEST(first > 0u);
	BOOST_TEST(assets->applied_through() == first);

	// A walk is running: the next batch waits behind it and does not count yet.
	assets->rebuild();
	BOOST_REQUIRE(assets->scanning());
	BOOST_REQUIRE(resources->store_sync(res::tag{ "res://y.txt" }, std::vector<std::byte>{ std::byte{ 'y' } }));
	tasks.drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(assets->scanning());
	BOOST_TEST(resources->delivering_batch() > first);
	BOOST_TEST(assets->applied_through() == first);

	pump();
	BOOST_TEST(!assets->scanning());
	BOOST_TEST(assets->applied_through() == resources->delivering_batch());
}

BOOST_AUTO_TEST_CASE(the_link_attaches_after_the_first_walk_and_detaches_when_it_goes)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	const res::reference by_id{ id_a(), res::tag{} };
	{
		edt::asset_index_link link(*assets, *resources);
		assets->rebuild();
		link.poll();
		BOOST_TEST(!link.attached());
		BOOST_TEST(!resources->resolve_reference(by_id).is_valid());

		pump();
		link.poll();
		BOOST_TEST(link.attached());
		BOOST_TEST(resources->resolve_reference(by_id) == res::tag{ "res://a.txt" });
	}
	BOOST_TEST(!resources->resolve_reference(by_id).is_valid());
}

BOOST_AUTO_TEST_CASE(a_file_moved_with_its_resource_keeps_its_loaded_block_and_pin)
{
	project.write("res/old/a.txt", "a");
	project.write("res/old/a.txt.resource", meta_text(k_guid_a));
	make_system();
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());

	const res::tag a{ "res://old/a.txt" };
	const res::tag b{ "res://new/a.txt" };
	resources->pin_resource(a, res::text_resource(a, std::vector<std::byte>{ std::byte{ 'p' } }));
	const auto before = resources->require_sync<res::text_resource>(a);
	BOOST_REQUIRE(before.get_sync());

	// Moved outside the editor with its .resource, both at once (their folder renamed): no poll of the watcher can see
	// one without the other.
	fs::rename(content / "old", content / "new");
	BOOST_REQUIRE(wait_for([&] {
		return link.index().path_of(id_a()) == std::optional{ b } && resources->waiting_for_index_count() == 0u &&
			!assets->scanning();
	}));

	BOOST_TEST(resources->require_sync<res::text_resource>(b).get_control_block() == before.get_control_block());
	BOOST_TEST(resources->pinned_count() == 1u);
	BOOST_TEST(resources->resolve_reference(res::reference{ id_a(), a }) == b);
}

BOOST_AUTO_TEST_CASE(a_move_a_walk_finds_ahead_of_its_batch_keeps_the_block_and_pin)
{
	project.write("res/old/a.txt", "a");
	project.write("res/old/a.txt.resource", meta_text(k_guid_a));
	make_system();
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());

	const res::tag a{ "res://old/a.txt" };
	const res::tag b{ "res://new/a.txt" };
	resources->pin_resource(a, res::text_resource(a, std::vector<std::byte>{ std::byte{ 'p' } }));
	const auto before = resources->require_sync<res::text_resource>(a);

	// A walk is queued; the watcher's batch of the move arrives while it is, and waits behind it. The walk then sees the
	// move first and pairs it under an earlier batch number than the files' own.
	assets->rebuild();
	const std::uint64_t delivered = resources->delivering_batch();
	fs::rename(content / "old", content / "new");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (resources->delivering_batch() == delivered && std::chrono::steady_clock::now() < deadline) {
		tasks.drain_main(std::chrono::hours(1));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	BOOST_REQUIRE(resources->delivering_batch() > delivered);
	BOOST_REQUIRE(assets->scanning());

	BOOST_REQUIRE(wait_for([&] {
		return link.index().path_of(id_a()) == std::optional{ b } && resources->waiting_for_index_count() == 0u &&
			!assets->scanning();
	}));
	BOOST_TEST(resources->require_sync<res::text_resource>(b).get_control_block() == before.get_control_block());
	BOOST_TEST(resources->pinned_count() == 1u);
}

BOOST_AUTO_TEST_CASE(dropped_changes_detach_the_index_until_the_next_walk)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	assets->set_max_deferred_for_tests(1);
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());
	const res::reference by_id{ id_a(), res::tag{} };
	BOOST_TEST(resources->resolve_reference(by_id) == res::tag{ "res://a.txt" });

	// Two changes behind a walk, one more than it keeps: they are dropped.
	assets->rebuild();
	res::file_change first;
	first.tag = res::tag{ "res://x.txt" };
	first.kind = res::change_kind::modified;
	res::file_change second = first;
	second.tag = res::tag{ "res://y.txt" };
	assets->apply_changes(std::vector<res::file_change>{ first, second });
	BOOST_TEST(assets->changes_dropped() == 1u);
	link.poll();
	BOOST_TEST(!link.attached());
	BOOST_TEST(!resources->resolve_reference(by_id).is_valid());

	pump();
	link.poll();
	BOOST_TEST(link.attached());
	BOOST_TEST(resources->resolve_reference(by_id) == res::tag{ "res://a.txt" });
}

// #866: a watcher's batch is applied for the IDs it touches; a file without an ID, or whose ID keeps its holder,
// changes nothing and copies nothing.
BOOST_AUTO_TEST_CASE(a_batch_of_one_file_leaves_the_other_answers_and_takes_no_full_refresh)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	project.write("res/b.txt", "b");
	project.write("res/b.txt.resource", meta_text(k_guid_b));
	project.write("res/plain.txt", "p");
	make_system();
	assets->rebuild();
	pump();
	edt::asset_index_snapshot snapshot;
	BOOST_TEST(snapshot.refresh(*assets));
	BOOST_TEST(snapshot.full_refreshes() == 1u);

	project.write("res/plain.txt", "plain, longer");
	BOOST_TEST(!snapshot.apply_batch(*assets, apply({ change("plain.txt", res::change_kind::modified) })));
	project.write("res/a.txt", "a, longer");
	BOOST_TEST(!snapshot.apply_batch(*assets, apply({ change("a.txt", res::change_kind::modified) })));
	BOOST_TEST(snapshot.full_refreshes() == 1u);
	BOOST_TEST((snapshot.id_of(res::tag{ "res://a.txt" }) == std::optional{ id_a() }));
	BOOST_TEST((snapshot.path_of(*res::asset_id_from_text(k_guid_b)) == std::optional{ res::tag{ "res://b.txt" } }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://plain.txt" }));
}

// #866: after an add, a move, a remove, a copy that carries an ID and the end of the file that held it, and an ID
// changed by its .resource, the batch-by-batch snapshot answers as a fresh one for every file. The copy is warned about
// once, also after it holds the ID.
BOOST_AUTO_TEST_CASE(batch_by_batch_answers_as_a_fresh_snapshot_after_every_kind_of_batch)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	project.write("res/b.txt", "b");
	project.write("res/b.txt.resource", meta_text(k_guid_b));
	make_system();
	assets->rebuild();
	pump();
	const res::asset_id a = id_a();
	const res::asset_id b = *res::asset_id_from_text(k_guid_b);
	const res::asset_id c = *res::asset_id_from_text(k_guid_c);
	const std::vector<res::asset_id> ids{ a, b, c };
	// The fresh snapshots warn of their own: only what the batch-by-batch one says is counted.
	std::size_t copy_warnings = 0;
	bool counting = true;
	const engine::log_sink_handle sink = engine::add_log_sink([&copy_warnings, &counting](std::string_view category,
		engine::log_level level, std::string_view message) {
		if (counting && category == "assets" && level == engine::log_level::warn
			&& message.starts_with("'res://d.txt' carries")) {
			++copy_warnings;
		}
	});
	edt::asset_index_snapshot snapshot;
	snapshot.refresh(*assets);
	const auto check = [&](const std::vector<res::tag>& gone) {
		counting = false;
		expect_as_fresh(snapshot, gone, ids);
		counting = true;
	};

	project.write("res/c.txt", "c");
	project.write("res/c.txt.resource", meta_text(k_guid_c));
	fs::last_write_time(content / "c.txt", fs::file_time_type::clock::now() - std::chrono::hours(2));
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("c.txt", res::change_kind::created),
		change("c.txt.resource", res::change_kind::created) })));
	check({});

	fs::create_directories(content / "moved");
	fs::rename(content / "a.txt", content / "moved/a.txt");
	fs::rename(content / "a.txt.resource", content / "moved/a.txt.resource");
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("moved/a.txt", res::change_kind::renamed, "a.txt"),
		change("moved/a.txt.resource", res::change_kind::renamed, "a.txt.resource") })));
	BOOST_TEST((snapshot.path_of(a) == std::optional{ res::tag{ "res://moved/a.txt" } }));
	check({ res::tag{ "res://a.txt" } });

	fs::remove(content / "b.txt");
	fs::remove(content / "b.txt.resource");
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("b.txt", res::change_kind::deleted),
		change("b.txt.resource", res::change_kind::deleted) })));
	BOOST_TEST(!snapshot.path_of(b));
	check({ res::tag{ "res://b.txt" } });

	// A newer copy with the .resource of c: c keeps the ID, the copy is warned about.
	project.write("res/d.txt", "c");
	project.write("res/d.txt.resource", meta_text(k_guid_c));
	fs::last_write_time(content / "d.txt.resource", fs::file_time_type::clock::now() - std::chrono::hours(1));
	BOOST_TEST(!snapshot.apply_batch(*assets, apply({ change("d.txt", res::change_kind::created),
		change("d.txt.resource", res::change_kind::created) })));
	BOOST_TEST(copy_warnings == 1u);
	check({});

	fs::remove(content / "c.txt");
	fs::remove(content / "c.txt.resource");
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("c.txt", res::change_kind::deleted),
		change("c.txt.resource", res::change_kind::deleted) })));
	BOOST_TEST((snapshot.path_of(c) == std::optional{ res::tag{ "res://d.txt" } }));
	check({ res::tag{ "res://c.txt" } });

	// The .resource of d now names b: c is held by none, b by d.
	rewrite_meta("d.txt", k_guid_b);
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("d.txt.resource", res::change_kind::modified) })));
	BOOST_TEST(!snapshot.path_of(c));
	BOOST_TEST((snapshot.path_of(b) == std::optional{ res::tag{ "res://d.txt" } }));
	check({});

	// Two files swap their IDs in one batch: each answer goes to the other path, none is lost.
	rewrite_meta("moved/a.txt", k_guid_b);
	rewrite_meta("d.txt", k_guid_a);
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("moved/a.txt.resource", res::change_kind::modified),
		change("d.txt.resource", res::change_kind::modified) })));
	BOOST_TEST((snapshot.path_of(a) == std::optional{ res::tag{ "res://d.txt" } }));
	BOOST_TEST((snapshot.path_of(b) == std::optional{ res::tag{ "res://moved/a.txt" } }));
	check({});

	// A file moved onto a path that held another ID, which goes in the same batch: the path answers the moved file's.
	// The moved files get stamps of their own: one written in the same clock tick as the file it replaces would be taken
	// as that file unchanged.
	for (const char* moved : { "moved/a.txt", "moved/a.txt.resource" }) {
		fs::last_write_time(content / moved, fs::file_time_type::clock::now() - std::chrono::hours(3));
	}
	fs::remove(content / "d.txt");
	fs::remove(content / "d.txt.resource");
	fs::rename(content / "moved/a.txt", content / "d.txt");
	fs::rename(content / "moved/a.txt.resource", content / "d.txt.resource");
	BOOST_TEST(snapshot.apply_batch(*assets, apply({ change("d.txt", res::change_kind::renamed, "moved/a.txt"),
		change("d.txt.resource", res::change_kind::renamed, "moved/a.txt.resource") })));
	BOOST_TEST((snapshot.id_of(res::tag{ "res://d.txt" }) == std::optional{ b }));
	BOOST_TEST(!snapshot.path_of(a));
	check({ res::tag{ "res://moved/a.txt" } });

	BOOST_TEST(copy_warnings == 1u);
	BOOST_TEST(snapshot.full_refreshes() == 1u);
}

// #866: the holders are read from the database as it is, so two batches applied in a row, the second moving the file
// the first added, answer as a fresh snapshot.
BOOST_AUTO_TEST_CASE(two_batches_in_a_row_the_second_moving_what_the_first_added_answer_as_fresh)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	assets->rebuild();
	pump();
	const res::asset_id c = *res::asset_id_from_text(k_guid_c);
	edt::asset_index_snapshot snapshot;
	snapshot.refresh(*assets);

	project.write("res/e.txt", "e");
	project.write("res/e.txt.resource", meta_text(k_guid_c));
	const edt::asset_batch added = apply({ change("e.txt", res::change_kind::created),
		change("e.txt.resource", res::change_kind::created) });
	fs::rename(content / "e.txt", content / "f.txt");
	fs::rename(content / "e.txt.resource", content / "f.txt.resource");
	const edt::asset_batch moved = apply({ change("f.txt", res::change_kind::renamed, "e.txt"),
		change("f.txt.resource", res::change_kind::renamed, "e.txt.resource") });

	snapshot.apply_batch(*assets, added);
	snapshot.apply_batch(*assets, moved);
	BOOST_TEST((snapshot.path_of(c) == std::optional{ res::tag{ "res://f.txt" } }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://e.txt" }));
	expect_as_fresh(snapshot, { res::tag{ "res://e.txt" } }, { id_a(), c });
	BOOST_TEST(snapshot.full_refreshes() == 1u);
}

// #866: the link takes a watcher's batch batch by batch and a walk's whole.
BOOST_AUTO_TEST_CASE(the_link_refreshes_the_whole_snapshot_after_a_walk_only)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());
	const std::size_t attached = link.index().full_refreshes();

	project.write("res/b.txt", "b");
	project.write("res/b.txt.resource", meta_text(k_guid_b));
	apply({ change("b.txt", res::change_kind::created), change("b.txt.resource", res::change_kind::created) });
	BOOST_TEST(link.index().full_refreshes() == attached);
	BOOST_TEST((link.index().path_of(*res::asset_id_from_text(k_guid_b)) == std::optional{ res::tag{ "res://b.txt" } }));

	project.write("res/c.txt", "c");
	project.write("res/c.txt.resource", meta_text(k_guid_c));
	assets->rebuild();
	BOOST_REQUIRE(wait_for([&] { return !assets->scanning(); }));
	BOOST_TEST(link.index().full_refreshes() > attached);
	BOOST_TEST((link.index().path_of(*res::asset_id_from_text(k_guid_c)) == std::optional{ res::tag{ "res://c.txt" } }));
}

// #885: a hash pass changes no answer of the index: poll() takes no whole snapshot after it, and a watcher's batch
// that follows is taken batch by batch.
BOOST_AUTO_TEST_CASE(a_hash_pass_takes_no_full_refresh)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());

	project.write("res/x.txt", "x");
	apply({ change("x.txt", res::change_kind::created) });
	link.poll();
	const std::size_t synced = link.index().full_refreshes();
	const std::uint64_t before_hash = assets->revision();
	const std::uint64_t ids_before_hash = assets->id_revision();
	BOOST_REQUIRE(wait_for([&] { return assets->pending_hashes() == 0u; }));
	BOOST_REQUIRE(assets->revision() > before_hash);
	BOOST_TEST(assets->id_revision() == ids_before_hash);
	link.poll();
	BOOST_TEST(link.index().full_refreshes() == synced);

	project.write("res/b.txt", "b");
	project.write("res/b.txt.resource", meta_text(k_guid_b));
	apply({ change("b.txt", res::change_kind::created), change("b.txt.resource", res::change_kind::created) });
	link.poll();
	BOOST_TEST(link.index().full_refreshes() == synced);
	BOOST_TEST((link.index().path_of(*res::asset_id_from_text(k_guid_b)) == std::optional{ res::tag{ "res://b.txt" } }));
}

BOOST_AUTO_TEST_SUITE_END()
