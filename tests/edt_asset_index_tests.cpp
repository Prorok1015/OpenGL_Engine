#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

// A project: res/ (the one content layer) and .cache/, with resource_system over them and the database following it.
struct project_fixture
{
	project_fixture()
	{
		fs::create_directories(content);
		fs::create_directories(cache_dir / "memory");
	}

	void make_system()
	{
		resources = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { content }, cache_dir, cache_dir / "user_profile"));
		assets = std::make_unique<edt::asset_database>(jobs, &events, resources->get_resource_layers());
		assets->attach(*resources);
	}

	void pump() { test_util::pump(tasks); }

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

BOOST_AUTO_TEST_CASE(the_snapshot_answers_for_the_older_of_two_files_with_one_id)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	project.write("res/copy.txt", "copy");
	project.write("res/copy.txt.resource", meta_text(k_guid_a));
	project.write("res/plain.txt", "p");
	fs::last_write_time(content / "a.txt", fs::file_time_type::clock::now() - std::chrono::hours(2));
	fs::last_write_time(content / "copy.txt", fs::file_time_type::clock::now() - std::chrono::hours(1));
	make_system();
	assets->rebuild();
	pump();

	edt::asset_index_snapshot snapshot;
	BOOST_TEST(snapshot.refresh(*assets));
	BOOST_TEST((snapshot.id_of(res::tag{ "res://a.txt" }) == std::optional{ id_a() }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://copy.txt" }));
	BOOST_TEST(!snapshot.id_of(res::tag{ "res://plain.txt" }));
	BOOST_TEST((snapshot.path_of(id_a()) == std::optional{ res::tag{ "res://a.txt" } }));
	// Nothing changed: the same answers, and the copy is not warned about again.
	BOOST_TEST(!snapshot.refresh(*assets));
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

BOOST_AUTO_TEST_CASE(a_file_moved_with_its_resource_keeps_its_loaded_block)
{
	project.write("res/a.txt", "a");
	project.write("res/a.txt.resource", meta_text(k_guid_a));
	make_system();
	edt::asset_index_link link(*assets, *resources);
	assets->rebuild();
	pump();
	link.poll();
	BOOST_REQUIRE(link.attached());

	const res::tag a{ "res://a.txt" };
	const res::tag b{ "res://b.txt" };
	const auto before = resources->require_sync<res::text_resource>(a);
	BOOST_REQUIRE(before.get_sync());

	// Moved outside the editor, with its .resource: one poll of the watcher sees both, the database pairs them by guid.
	fs::rename(content / "a.txt", content / "b.txt");
	fs::rename(content / "a.txt.resource", content / "b.txt.resource");
	BOOST_REQUIRE(wait_for([&] {
		return link.index().path_of(id_a()) == std::optional{ b } && resources->waiting_for_index_count() == 0u;
	}));

	BOOST_TEST(resources->require_sync<res::text_resource>(b).get_control_block() == before.get_control_block());
	BOOST_TEST(resources->resolve_reference(res::reference{ id_a(), a }) == b);
}

BOOST_AUTO_TEST_SUITE_END()
