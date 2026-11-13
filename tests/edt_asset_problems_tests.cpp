#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_problems.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/resource/res_asset_meta.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The source "assets" of Problems (S57-4): its rows, its fixes and their undo.

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

res::file_change change(std::string_view path, res::change_kind kind)
{
	res::file_change out;
	out.tag = res::tag::make(path);
	out.kind = kind;
	return out;
}

std::string res_path(std::string_view relative)
{
	return res::tag::make(relative).string();
}

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

constexpr std::string_view k_guid_a = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_guid_b = "00000000-0000-4000-8000-000000000002";

struct asset_problems_fixture
{
	asset_problems_fixture() : lower("asset_problems_lower"), upper("asset_problems_upper") {}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ lower.path(), upper.path() });
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	// The rows of the source after a publish.
	std::vector<edt::problem> rows()
	{
		publisher.publish(problems, *db);
		std::vector<edt::problem> out;
		for (const edt::sourced_problem& row : problems.list()) {
			if (row.source == edt::asset_problem_publisher::source) {
				out.push_back(row.item);
			}
		}
		return out;
	}

	std::vector<edt::problem> rows_of(std::string_view code)
	{
		std::vector<edt::problem> out;
		for (edt::problem& item : rows()) {
			if (item.code == code) {
				out.push_back(std::move(item));
			}
		}
		return out;
	}

	std::optional<edt::problem> row(std::string_view code, std::string_view relative)
	{
		for (edt::problem& found : rows_of(code)) {
			if (found.file == res_path(relative)) {
				return found;
			}
		}
		return std::nullopt;
	}

	const edt::asset_record& record(std::string_view path) const
	{
		const edt::asset_record* found = db->find(res::tag::make(path));
		BOOST_REQUIRE_MESSAGE(found, std::string{ path });
		return *found;
	}

	static void set_time(const fs::path& path, std::chrono::hours ago)
	{
		fs::last_write_time(path, fs::file_time_type::clock::now() - ago);
	}

	test_util::temp_dir lower;
	test_util::temp_dir upper;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
	edt::problem_list problems;
	edt::asset_problem_publisher publisher;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetProblems, asset_problems_fixture)

BOOST_AUTO_TEST_CASE(each_kind_of_problem_has_its_row)
{
	lower.write("engine.png", "engine");
	upper.write("plain.png", "plain");
	upper.write("bad.png", "bad");
	upper.write("bad.png.resource", "{ not json");
	upper.write("lost.png.resource", meta_text(k_guid_a));
	set_time(upper.write("one.png", "one"), std::chrono::hours(2));
	upper.write("one.png.resource", meta_text(k_guid_b));
	set_time(upper.write("two.png", "two"), std::chrono::hours(1));
	upper.write("two.png.resource", meta_text(k_guid_b));
	open();

	const auto no_id = rows_of("asset.no_id");
	BOOST_REQUIRE_EQUAL(no_id.size(), 1u);
	BOOST_TEST(no_id[0].file == res_path("plain.png"));
	BOOST_TEST((no_id[0].severity == edt::problem_severity::warning));

	const auto bad = row("asset.bad_meta", "bad.png");
	BOOST_REQUIRE(bad);
	BOOST_TEST((bad->severity == edt::problem_severity::warning));
	BOOST_TEST(publisher.actions_of(*bad, *db).empty());

	const auto orphan = row("asset.orphan", "lost.png.resource");
	BOOST_REQUIRE(orphan);
	BOOST_TEST((orphan->severity == edt::problem_severity::warning));
	BOOST_TEST(publisher.actions_of(*orphan, *db).empty());

	const auto duplicates = rows_of("asset.duplicate_id");
	BOOST_REQUIRE_EQUAL(duplicates.size(), 1u);
	BOOST_TEST(duplicates[0].file == res_path("two.png"));
	BOOST_TEST((duplicates[0].severity == edt::problem_severity::error));
	BOOST_TEST(duplicates[0].message.find(res_path("one.png")) != std::string::npos);

	BOOST_TEST((publisher.file_of(*orphan) == res_path("lost.png.resource")));
}

BOOST_AUTO_TEST_CASE(nothing_wrong_clears_the_source)
{
	upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_a));
	open();
	BOOST_TEST(rows().empty());
	BOOST_TEST(problems.size() == 0u);
}

BOOST_AUTO_TEST_CASE(create_resource_gives_the_file_an_id_and_undo_takes_it_back)
{
	upper.write("plain.png", "plain");
	open();
	const auto found = row("asset.no_id", "plain.png");
	BOOST_REQUIRE(found);
	auto actions = publisher.actions_of(*found, *db);
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].label == "Create .resource");

	const edt::asset_fix_result created = actions[0].run();
	BOOST_REQUIRE_MESSAGE(created.ok, created.error);
	BOOST_TEST(!row("asset.no_id", "plain.png"));
	BOOST_TEST(record("plain.png").id.has_value());
	BOOST_TEST(fs::is_regular_file(upper.path() / "plain.png.resource"));
	BOOST_TEST(db->next_undo_label().has_value());

	const edt::asset_fix_result undone = db->undo_last();
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(!fs::exists(upper.path() / "plain.png.resource"));
	BOOST_TEST(!record("plain.png").id.has_value());
	BOOST_TEST(row("asset.no_id", "plain.png").has_value());
	BOOST_TEST(!db->next_undo_label().has_value());
}

BOOST_AUTO_TEST_CASE(create_resource_refuses_a_file_of_a_lower_layer)
{
	lower.write("engine.png", "engine");
	open();
	BOOST_TEST(rows().empty());
	BOOST_TEST(!db->create_meta(res::tag::make("engine.png")).ok);
	BOOST_TEST(!fs::exists(lower.path() / "engine.png.resource"));
}

BOOST_AUTO_TEST_CASE(an_orphan_links_to_the_file_with_its_last_content)
{
	upper.write("a.png", "same bytes");
	upper.write("a.png.resource", meta_text(k_guid_a));
	open();
	const std::string hash = record("a.png").content_hash;
	BOOST_REQUIRE(!hash.empty());

	fs::remove(upper.path() / "a.png");
	db->apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted) });
	BOOST_REQUIRE_EQUAL(db->orphans().size(), 1u);
	BOOST_TEST(db->orphans()[0].last_hash == hash);
	BOOST_TEST(db->orphans()[0].layer == 1u);

	upper.write("b.png", "same bytes");
	db->apply_changes(std::vector<res::file_change>{ change("b.png", res::change_kind::created) });
	test_util::pump(tasks);
	BOOST_REQUIRE(record("b.png").content_hash == hash);

	const auto orphan = row("asset.orphan", "a.png.resource");
	BOOST_REQUIRE(orphan);
	auto actions = publisher.actions_of(*orphan, *db);
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].label == "Link to " + res_path("b.png"));

	const edt::asset_fix_result linked = actions[0].run();
	BOOST_REQUIRE_MESSAGE(linked.ok, linked.error);
	BOOST_TEST((record("b.png").id == res::asset_id_from_text(k_guid_a)));
	BOOST_TEST(db->orphans().empty());
	BOOST_TEST(fs::is_regular_file(upper.path() / "b.png.resource"));
	BOOST_TEST(!fs::exists(upper.path() / "a.png.resource"));
	BOOST_TEST(!row("asset.orphan", "a.png.resource"));

	const edt::asset_fix_result undone = db->undo_last();
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_REQUIRE_EQUAL(db->orphans().size(), 1u);
	BOOST_TEST((db->orphans()[0].meta_path == res::tag::make("a.png.resource")));
	BOOST_TEST(db->orphans()[0].last_hash == hash);
	BOOST_TEST(!record("b.png").id.has_value());
	const auto again = row("asset.orphan", "a.png.resource");
	BOOST_REQUIRE(again);
	BOOST_TEST(publisher.actions_of(*again, *db).size() == 1u);
}

BOOST_AUTO_TEST_CASE(an_orphan_of_a_lower_layer_offers_no_link)
{
	lower.write("a.png", "same bytes");
	lower.write("a.png.resource", meta_text(k_guid_a));
	open();
	const std::string hash = record("a.png").content_hash;
	BOOST_REQUIRE(!hash.empty());

	fs::remove(lower.path() / "a.png");
	db->apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted) });
	BOOST_REQUIRE_EQUAL(db->orphans().size(), 1u);
	BOOST_TEST(db->orphans()[0].layer == 0u);
	BOOST_TEST(db->orphans()[0].last_hash == hash);

	upper.write("b.png", "same bytes");
	db->apply_changes(std::vector<res::file_change>{ change("b.png", res::change_kind::created) });
	test_util::pump(tasks);
	BOOST_REQUIRE(record("b.png").content_hash == hash);

	const auto orphan = row("asset.orphan", "a.png.resource");
	BOOST_REQUIRE(orphan);
	BOOST_TEST(publisher.actions_of(*orphan, *db).empty());
	BOOST_TEST(!db->link_orphan(res::tag::make("a.png.resource"), res::tag::make("b.png")).ok);
	BOOST_TEST(fs::is_regular_file(lower.path() / "a.png.resource"));
	BOOST_TEST(!fs::exists(upper.path() / "b.png.resource"));
}

BOOST_AUTO_TEST_CASE(an_orphan_whose_content_two_files_hold_offers_no_link)
{
	upper.write("a.png", "same bytes");
	upper.write("a.png.resource", meta_text(k_guid_a));
	open();
	fs::remove(upper.path() / "a.png");
	db->apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted) });
	upper.write("b.png", "same bytes");
	upper.write("c.png", "same bytes");
	db->apply_changes(std::vector<res::file_change>{ change("b.png", res::change_kind::created), change("c.png", res::change_kind::created) });
	test_util::pump(tasks);
	BOOST_REQUIRE(record("b.png").content_hash == record("c.png").content_hash);
	BOOST_REQUIRE_EQUAL(db->orphans().size(), 1u);
	BOOST_REQUIRE(db->orphans()[0].last_hash == record("b.png").content_hash);

	const auto orphan = row("asset.orphan", "a.png.resource");
	BOOST_REQUIRE(orphan);
	BOOST_TEST(publisher.actions_of(*orphan, *db).empty());
	BOOST_TEST(!db->link_orphan(res::tag::make("a.png.resource"), res::tag::make("b.png")).ok);
	BOOST_TEST(fs::is_regular_file(upper.path() / "a.png.resource"));
}

BOOST_AUTO_TEST_CASE(an_orphan_without_a_known_hash_offers_no_link)
{
	upper.write("lost.png.resource", meta_text(k_guid_a));
	upper.write("free.png", "free");
	open();
	const auto orphan = row("asset.orphan", "lost.png.resource");
	BOOST_REQUIRE(orphan);
	BOOST_TEST(publisher.actions_of(*orphan, *db).empty());
}

BOOST_AUTO_TEST_CASE(the_newer_copy_gets_a_new_id_and_undo_brings_the_duplicate_back)
{
	set_time(upper.write("one.png", "one"), std::chrono::hours(2));
	upper.write("one.png.resource", meta_text(k_guid_b));
	set_time(upper.write("two.png", "two"), std::chrono::hours(1));
	upper.write("two.png.resource", meta_text(k_guid_b));
	open();
	BOOST_TEST(!row("asset.duplicate_id", "one.png"));
	const auto copy = row("asset.duplicate_id", "two.png");
	BOOST_REQUIRE(copy);
	auto actions = publisher.actions_of(*copy, *db);
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].label == "New ID for this copy");

	const edt::asset_fix_result renewed = actions[0].run();
	BOOST_REQUIRE_MESSAGE(renewed.ok, renewed.error);
	BOOST_TEST(db->duplicates().empty());
	BOOST_TEST((record("one.png").id == res::asset_id_from_text(k_guid_b)));
	BOOST_REQUIRE(record("two.png").id.has_value());
	BOOST_TEST((record("two.png").id != res::asset_id_from_text(k_guid_b)));
	BOOST_TEST(rows_of("asset.duplicate_id").empty());
	const res::asset_meta_result kept = res::read_asset_meta(read_text(upper.path() / "two.png.resource"));
	BOOST_REQUIRE(kept);
	BOOST_TEST(kept.meta->importer == "test");
	BOOST_TEST((kept.meta->flags == std::vector<std::string>{ "editor_only" }));

	const edt::asset_fix_result undone = db->undo_last();
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(db->duplicates().size() == 1u);
	BOOST_TEST(read_text(upper.path() / "two.png.resource") == meta_text(k_guid_b));
	BOOST_TEST(row("asset.duplicate_id", "two.png").has_value());
}

BOOST_AUTO_TEST_CASE(with_equal_times_the_larger_path_is_the_copy)
{
	const fs::path a = upper.write("a.png", "a");
	upper.write("a.png.resource", meta_text(k_guid_b));
	const fs::path b = upper.write("b.png", "b");
	upper.write("b.png.resource", meta_text(k_guid_b));
	const auto when = fs::file_time_type::clock::now() - std::chrono::hours(1);
	fs::last_write_time(a, when);
	fs::last_write_time(b, when);
	open();
	const auto duplicates = rows_of("asset.duplicate_id");
	BOOST_REQUIRE_EQUAL(duplicates.size(), 1u);
	BOOST_TEST(duplicates[0].file == res_path("b.png"));
}

BOOST_AUTO_TEST_CASE(the_lower_layer_holds_the_original_even_when_newer)
{
	set_time(upper.write("one.png", "one"), std::chrono::hours(2));
	upper.write("one.png.resource", meta_text(k_guid_b));
	set_time(lower.write("two.png", "two"), std::chrono::hours(1));
	lower.write("two.png.resource", meta_text(k_guid_b));
	open();
	BOOST_TEST(!row("asset.duplicate_id", "two.png"));
	const auto copy = row("asset.duplicate_id", "one.png");
	BOOST_REQUIRE(copy);
	BOOST_TEST((copy->severity == edt::problem_severity::error));
	BOOST_TEST(copy->message.find(res_path("two.png")) != std::string::npos);
	const auto actions = publisher.actions_of(*copy, *db);
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].label == "New ID for this copy");
}

BOOST_AUTO_TEST_CASE(a_duplicate_wholly_below_the_project_is_a_warning_without_a_fix)
{
	set_time(lower.write("one.png", "one"), std::chrono::hours(2));
	lower.write("one.png.resource", meta_text(k_guid_b));
	set_time(lower.write("two.png", "two"), std::chrono::hours(1));
	lower.write("two.png.resource", meta_text(k_guid_b));
	open();
	const auto duplicates = rows_of("asset.duplicate_id");
	BOOST_REQUIRE_EQUAL(duplicates.size(), 1u);
	BOOST_TEST(duplicates[0].file == res_path("two.png"));
	BOOST_TEST((duplicates[0].severity == edt::problem_severity::warning));
	BOOST_TEST(publisher.actions_of(duplicates[0], *db).empty());
}

BOOST_AUTO_TEST_CASE(a_copy_whose_resource_comes_from_a_lower_layer_offers_no_new_id)
{
	set_time(lower.write("one.png", "one"), std::chrono::hours(2));
	lower.write("one.png.resource", meta_text(k_guid_b));
	set_time(upper.write("two.png", "two"), std::chrono::hours(1));
	lower.write("two.png.resource", meta_text(k_guid_b));
	open();
	BOOST_TEST(record("two.png").layer == 1u);
	BOOST_TEST(record("two.png").meta_layer == 0u);
	BOOST_TEST(!row("asset.no_id", "two.png"));
	const auto copy = row("asset.duplicate_id", "two.png");
	BOOST_REQUIRE(copy);
	BOOST_TEST(publisher.actions_of(*copy, *db).empty());
	BOOST_TEST(!db->new_id_for_copy(res::tag::make("two.png")).ok);
	BOOST_TEST(read_text(lower.path() / "two.png.resource") == meta_text(k_guid_b));
}

BOOST_AUTO_TEST_CASE(a_link_is_not_undone_once_the_moved_resource_changed)
{
	upper.write("a.png", "same bytes");
	upper.write("a.png.resource", meta_text(k_guid_a));
	open();
	fs::remove(upper.path() / "a.png");
	db->apply_changes(std::vector<res::file_change>{ change("a.png", res::change_kind::deleted) });
	upper.write("b.png", "same bytes");
	db->apply_changes(std::vector<res::file_change>{ change("b.png", res::change_kind::created) });
	test_util::pump(tasks);
	BOOST_REQUIRE(db->link_orphan(res::tag::make("a.png.resource"), res::tag::make("b.png")).ok);

	upper.write("b.png.resource", meta_text(k_guid_b));
	const edt::asset_fix_result undone = db->undo_last();
	BOOST_TEST(!undone.ok);
	BOOST_TEST(!undone.keep);
	BOOST_TEST(read_text(upper.path() / "b.png.resource") == meta_text(k_guid_b));
	BOOST_TEST(!fs::exists(upper.path() / "a.png.resource"));
	BOOST_TEST(!db->next_undo_label().has_value());
}

BOOST_AUTO_TEST_CASE(a_fix_during_a_walk_shows_once_the_walk_ends)
{
	set_time(upper.write("one.png", "one"), std::chrono::hours(2));
	upper.write("one.png.resource", meta_text(k_guid_b));
	set_time(upper.write("two.png", "two"), std::chrono::hours(1));
	upper.write("two.png.resource", meta_text(k_guid_b));
	upper.write("plain.png", "plain");
	open();
	db->rebuild();
	BOOST_REQUIRE(db->scanning());

	BOOST_REQUIRE(db->create_meta(res::tag::make("plain.png")).ok);
	BOOST_REQUIRE(db->new_id_for_copy(res::tag::make("two.png")).ok);
	BOOST_TEST(fs::is_regular_file(upper.path() / "plain.png.resource"));
	test_util::pump(tasks);

	BOOST_TEST(record("plain.png").id.has_value());
	BOOST_TEST(db->duplicates().empty());
	BOOST_TEST(!row("asset.no_id", "plain.png"));
	BOOST_TEST(rows_of("asset.duplicate_id").empty());
	BOOST_REQUIRE(db->undo_last().ok);
	test_util::pump(tasks);
	BOOST_TEST(db->duplicates().size() == 1u);
}

BOOST_AUTO_TEST_CASE(the_history_keeps_the_last_32_actions)
{
	constexpr std::size_t count = edt::asset_database::max_history + 1;
	for (std::size_t i = 0; i < count; ++i) {
		upper.write(std::format("f{:02}.png", i), std::format("file {}", i));
	}
	open();
	for (std::size_t i = 0; i < count; ++i) {
		BOOST_REQUIRE(db->create_meta(res::tag::make(std::format("f{:02}.png", i))).ok);
	}
	std::size_t undone = 0;
	while (db->next_undo_label()) {
		BOOST_REQUIRE(db->undo_last().ok);
		++undone;
	}
	BOOST_TEST(undone == edt::asset_database::max_history);
	// The oldest action is gone from the history: its .resource stays.
	BOOST_TEST(fs::is_regular_file(upper.path() / "f00.png.resource"));
	BOOST_TEST(!fs::exists(upper.path() / "f01.png.resource"));
	BOOST_TEST(!db->undo_last().ok);
}

BOOST_AUTO_TEST_CASE(an_undo_that_cannot_read_stays_in_the_history)
{
	upper.write("plain.png", "plain");
	open();
	BOOST_REQUIRE(db->create_meta(res::tag::make("plain.png")).ok);
	const fs::path meta = upper.path() / "plain.png.resource";
	const std::string written = read_text(meta);
	// A folder at the name cannot be read: not a change of the file, so the undo is kept to be tried again.
	fs::remove(meta);
	fs::create_directory(meta);

	const edt::asset_fix_result failed = db->undo_last();
	BOOST_TEST(!failed.ok);
	BOOST_TEST(failed.keep);
	BOOST_TEST(db->next_undo_label().has_value());

	fs::remove(meta);
	upper.write("plain.png.resource", written);
	const edt::asset_fix_result undone = db->undo_last();
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(!fs::exists(meta));
}

#if !defined(_WIN32)
namespace {
// Takes the write permission off a folder for the scope; false when it still takes a file (run as root).
struct read_only_folder
{
	explicit read_only_folder(fs::path folder) : path(std::move(folder))
	{
		fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
	}
	~read_only_folder()
	{
		std::error_code ec;
		fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ec);
	}
	bool holds() const
	{
		const fs::path probe = path / "probe.tmp";
		const bool made = static_cast<bool>(std::ofstream(probe));
		std::error_code ec;
		fs::remove(probe, ec);
		return !made;
	}
	fs::path path;
};
}

BOOST_AUTO_TEST_CASE(an_undo_that_cannot_write_or_rename_stays_in_the_history)
{
	set_time(upper.write("sub/one.png", "one"), std::chrono::hours(2));
	upper.write("sub/one.png.resource", meta_text(k_guid_b));
	set_time(upper.write("sub/two.png", "two"), std::chrono::hours(1));
	upper.write("sub/two.png.resource", meta_text(k_guid_b));
	upper.write("sub/a.png", "same bytes");
	upper.write("sub/a.png.resource", meta_text(k_guid_a));
	open();
	BOOST_REQUIRE(db->new_id_for_copy(res::tag::make("sub/two.png")).ok);
	{
		read_only_folder guard(upper.path() / "sub");
		if (!guard.holds()) {
			BOOST_TEST_MESSAGE("the folder stays writable (root?): the write failure is not tested");
			return;
		}
		const edt::asset_fix_result failed = db->undo_last();
		BOOST_TEST(!failed.ok);
		BOOST_TEST(failed.keep);
		BOOST_TEST(db->next_undo_label().has_value());
	}
	BOOST_REQUIRE(db->undo_last().ok);
	BOOST_TEST(db->duplicates().size() == 1u);

	fs::remove(upper.path() / "sub/a.png");
	db->apply_changes(std::vector<res::file_change>{ change("sub/a.png", res::change_kind::deleted) });
	upper.write("sub/b.png", "same bytes");
	db->apply_changes(std::vector<res::file_change>{ change("sub/b.png", res::change_kind::created) });
	test_util::pump(tasks);
	BOOST_REQUIRE(db->link_orphan(res::tag::make("sub/a.png.resource"), res::tag::make("sub/b.png")).ok);
	{
		read_only_folder guard(upper.path() / "sub");
		const edt::asset_fix_result failed = db->undo_last();
		BOOST_TEST(!failed.ok);
		BOOST_TEST(failed.keep);
		BOOST_TEST(fs::is_regular_file(upper.path() / "sub/b.png.resource"));
	}
	BOOST_REQUIRE(db->undo_last().ok);
	BOOST_TEST(fs::is_regular_file(upper.path() / "sub/a.png.resource"));
}
#endif

BOOST_AUTO_TEST_CASE(undo_is_refused_when_the_file_changed_since)
{
	upper.write("plain.png", "plain");
	open();
	BOOST_REQUIRE(db->create_meta(res::tag::make("plain.png")).ok);
	upper.write("plain.png.resource", "changed by hand");

	const edt::asset_fix_result undone = db->undo_last();
	BOOST_TEST(!undone.ok);
	BOOST_TEST(!undone.error.empty());
	BOOST_TEST(read_text(upper.path() / "plain.png.resource") == "changed by hand");
	BOOST_TEST(!db->next_undo_label().has_value());
}

BOOST_AUTO_TEST_CASE(create_resource_never_replaces_one_written_meanwhile)
{
	upper.write("plain.png", "plain");
	open();
	upper.write("plain.png.resource", meta_text(k_guid_a));
	BOOST_TEST(!db->create_meta(res::tag::make("plain.png")).ok);
	BOOST_TEST(read_text(upper.path() / "plain.png.resource") == meta_text(k_guid_a));
	BOOST_TEST(!db->next_undo_label().has_value());
}

BOOST_AUTO_TEST_SUITE_END()
