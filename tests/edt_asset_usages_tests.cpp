#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_usages.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Who refers to an asset (#774, EPIC-57 section 6.3): the references the asset database reads from the files on the disk,
// looked up backwards, and asset.usages over them.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::string_view k_sky_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_gone_guid = "00000000-0000-4000-8000-000000000009";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	return res::write_asset_meta(meta);
}

// A level that refers to sky.desc in every way a file can, and a material that inherits from it.
const char* const k_level = R"({
	"__type": "level_desc",
	"worlds": [ { "__type": "world_desc", "name": "w", "children": [ { "__id": "00000000000000a1",
		"components": {
			"sky": "res://sky.desc",
			"mesh": { "__type": "mesh_node_desc", "material": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://old_sky.desc" } },
			"inst": { "__type": "prefab_desc", "__parent": "res://sky.desc" },
			"lost": { "__type": "mesh_node_desc", "material": { "guid": "00000000-0000-4000-8000-000000000009", "path": "res://sky.desc" } },
			"other": { "__type": "mesh_node_desc", "material": "res://other.desc" }
		} } ] } ],
	"web": "https://example.com/sky.desc"
})";

struct usages_fixture
{
	usages_fixture() : layer("asset_usages"), cache("asset_usages_cache")
	{
		write("sky.desc", R"({ "__type": "material_desc" })");
		write("sky.desc.resource", meta_text(k_sky_guid));
		write("levels/main.desc", k_level);
		write("materials/child.desc", R"({ "__type": "material_desc", "__parent": "res://sky.desc" })");
	}

	// Older than the stamp granularity, so a cache trusts what it learned of the file.
	void write(std::string_view relative, std::string_view text)
	{
		const fs::path path = layer.write(relative, text);
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	edt::asset_database& open(bool with_cache = false)
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ layer.path() },
			with_cache ? edt::asset_cache_location{ cache.path(), "asset_db" } : edt::asset_cache_location{});
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	edt::usage_report sky_usages() const
	{
		return edt::find_usages(*db, nullptr, edt::usage_target{ res::tag::make("sky.desc"), std::nullopt });
	}

	static const edt::asset_usage* at(const edt::usage_report& report, std::string_view file, std::string_view pointer)
	{
		const auto found = std::ranges::find_if(report.items, [&](const edt::asset_usage& usage) {
			return usage.file == file && usage.json_pointer == pointer;
		});
		return found == report.items.end() ? nullptr : &*found;
	}

	test_util::temp_dir layer;
	test_util::temp_dir cache;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
};

const std::string k_node = "/worlds/0/children/0/components";

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetUsagesTests, usages_fixture)

BOOST_AUTO_TEST_CASE(EveryReferenceToTheAsset_IsFoundWithItsFileAndPointer)
{
	open();
	const edt::usage_report report = sky_usages();
	BOOST_TEST(report.complete);
	BOOST_REQUIRE(report.id.has_value());
	BOOST_TEST(res::to_text(*report.id) == k_sky_guid);

	const edt::asset_usage* component = at(report, "res://levels/main.desc", k_node + "/sky");
	BOOST_REQUIRE(component);
	BOOST_TEST((component->kind == edt::usage_kind::component));
	BOOST_TEST(!component->by_guid);

	const edt::asset_usage* by_guid = at(report, "res://levels/main.desc", k_node + "/mesh/material");
	BOOST_REQUIRE_MESSAGE(by_guid, "a {guid, path} with an old path is found by its guid");
	BOOST_TEST(by_guid->by_guid);

	const edt::asset_usage* instance = at(report, "res://levels/main.desc", k_node + "/inst/__parent");
	BOOST_REQUIRE(instance);
	BOOST_TEST((instance->kind == edt::usage_kind::parent));

	const edt::asset_usage* unknown_guid = at(report, "res://levels/main.desc", k_node + "/lost/material");
	BOOST_REQUIRE_MESSAGE(unknown_guid, "a guid no file holds: its path counts");
	BOOST_TEST(!unknown_guid->by_guid);

	const edt::asset_usage* inherited = at(report, "res://materials/child.desc", "/__parent");
	BOOST_REQUIRE(inherited);
	BOOST_TEST((inherited->kind == edt::usage_kind::parent));

	BOOST_TEST(!at(report, "res://levels/main.desc", k_node + "/other/material"));
	BOOST_TEST(!at(report, "res://levels/main.desc", "/web"));
	BOOST_TEST(report.items.size() == 5u);

	const edt::usage_report by_id = edt::find_usages(*db, nullptr, edt::usage_target{ std::nullopt, res::asset_id_from_text(k_sky_guid) });
	BOOST_REQUIRE(by_id.path.has_value());
	BOOST_TEST(by_id.path->view() == "res://sky.desc");
	BOOST_TEST(by_id.items.size() == report.items.size());
}

BOOST_AUTO_TEST_CASE(AnEditOfTheReferringFile_UpdatesTheUsages)
{
	auto& assets = open();
	BOOST_TEST(at(sky_usages(), "res://materials/child.desc", "/__parent") != nullptr);

	write("materials/child.desc", R"({ "__type": "material_desc", "__parent": "res://other.desc" })");
	res::file_change change;
	change.tag = res::tag::make("materials/child.desc");
	change.kind = res::change_kind::modified;
	assets.apply_changes(std::vector<res::file_change>{ change });
	test_util::pump(tasks);
	BOOST_TEST(at(sky_usages(), "res://materials/child.desc", "/__parent") == nullptr);

	write("materials/child.desc", R"({ "__type": "material_desc", "__parent": "res://sky.desc", "note": "x" })");
	assets.apply_changes(std::vector<res::file_change>{ change });
	test_util::pump(tasks);
	BOOST_TEST(at(sky_usages(), "res://materials/child.desc", "/__parent") != nullptr);
}

BOOST_AUTO_TEST_CASE(TheReferences_ComeBackFromTheCache_AndAnOlderCacheIsDropped)
{
	{
		auto& first = open(true);
		first.save();
		BOOST_TEST(sky_usages().items.size() == 5u);
	}
	db.reset();
	auto& second = open(true);
	BOOST_TEST(second.loaded_from_cache());
	BOOST_TEST(second.files_read() == 0u, "nothing is read again: the references come from the cache");
	BOOST_TEST(sky_usages().items.size() == 5u);

	// A cache of version 3 has no references: it is dropped and every file is read again.
	db.reset();
	const fs::path file = cache.path() / "asset_db" / "assets.json";
	std::string text;
	{
		std::ifstream in(file, std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	json::value document = json::parse(text);
	document.as_object()["__version"] = 3;
	std::ofstream(file, std::ios::binary | std::ios::trunc) << json::serialize(document);
	auto& third = open(true);
	BOOST_TEST(!third.loaded_from_cache());
	BOOST_TEST(sky_usages().items.size() == 5u);
}

BOOST_AUTO_TEST_CASE(ADescBiggerThanTheLimit_IsSkipped)
{
	const edt::asset_content content = edt::read_asset_content(layer.path(), "levels/main.desc", true, 16);
	BOOST_TEST(content.ok);
	BOOST_TEST(content.references_skipped);
	BOOST_TEST(content.references.empty());
	const edt::asset_content whole = edt::read_asset_content(layer.path(), "levels/main.desc", true, edt::asset_database::max_desc_bytes);
	BOOST_TEST(!whole.references_skipped);
	BOOST_TEST(whole.references.size() == 6u);
}

BOOST_AUTO_TEST_CASE(TheManifest_IsAUsageToo)
{
	open();
	const std::vector<edt::manifest_reference> manifest{ { "/start_scene", "res://sky.desc" }, { "/input_actions", "res://input.desc" } };
	const edt::usage_report report = edt::find_usages(*db, nullptr, edt::usage_target{ res::tag::make("sky.desc"), std::nullopt },
		"game.snkproj", manifest);
	const edt::asset_usage* start = at(report, "game.snkproj", "/start_scene");
	BOOST_REQUIRE(start);
	BOOST_TEST((start->kind == edt::usage_kind::manifest));
	BOOST_TEST(at(report, "game.snkproj", "/input_actions") == nullptr);
}

BOOST_AUTO_TEST_CASE(AssetUsages_AnswersByPathOrGuid_PageByPage)
{
	open();
	cmd::command_registry commands;
	edt::asset_usage_sources sources;
	sources.assets = [this]() -> const edt::asset_database* { return db.get(); };
	sources.manifest_file = [] { return std::string("game.snkproj"); };
	sources.manifest = [] { return std::vector<edt::manifest_reference>{ { "/start_scene", "res://sky.desc" } }; };
	const auto registered = edt::register_asset_usage_commands(commands, std::move(sources));
	BOOST_REQUIRE(registered.size() == 1u);

	const cmd::result by_path = commands.invoke("asset.usages", json::object{ { "path", "res://sky.desc" } });
	BOOST_REQUIRE_MESSAGE(by_path.ok, by_path.error);
	BOOST_TEST(std::string(by_path.value.at("source").as_string()) == "disk");
	BOOST_TEST(by_path.value.at("complete").as_bool());
	BOOST_TEST(std::string(by_path.value.at("target").at("guid").as_string()) == k_sky_guid);
	BOOST_TEST(by_path.value.at("items").as_array().size() == 6u);
	BOOST_TEST(by_path.value.at("skipped").as_array().empty());

	const cmd::result by_guid = commands.invoke("asset.usages", json::object{ { "guid", std::string(k_sky_guid) } });
	BOOST_REQUIRE_MESSAGE(by_guid.ok, by_guid.error);
	BOOST_TEST(std::string(by_guid.value.at("target").at("path").as_string()) == "res://sky.desc");
	BOOST_TEST(by_guid.value.at("items") == by_path.value.at("items"));

	// Page by page, the same rows in the same order.
	json::array rows;
	json::object args{ { "path", "sky.desc" }, { "page_size", 2 } };
	for (int page = 0; page < 10; ++page) {
		const cmd::result next = commands.invoke("asset.usages", args);
		BOOST_REQUIRE_MESSAGE(next.ok, next.error);
		for (const json::value& row : next.value.at("items").as_array()) rows.push_back(row);
		const json::value* cursor = next.value.as_object().if_contains("next_cursor");
		if (!cursor) break;
		args["cursor"] = *cursor;
	}
	BOOST_TEST(json::value(rows) == by_path.value.at("items"));

	BOOST_TEST(commands.invoke("asset.usages", json::object{}).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("asset.usages", json::object{ { "path", "res://sky.desc" }, { "guid", std::string(k_sky_guid) } }).code
		== cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("asset.usages", json::object{ { "path", "engine://sky.desc" } }).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("asset.usages", json::object{ { "guid", "not-a-guid" } }).code == cmd::codes::invalid_args);
}

// "Used by" in the asset browser asks the database only when another asset is picked or the database moved, not at every
// frame; without a database (no project) it says so by giving no report.
BOOST_AUTO_TEST_CASE(TheUsedByList_AsksAgainOnlyWhenTheDatabaseMovedOrAnotherAssetIsPicked)
{
	auto& assets = open();
	const res::tag sky = res::tag::make("sky.desc");
	edt::usage_list list;
	BOOST_TEST(list.report_for(sky) == nullptr);

	list.set_source([this](const res::tag& asset) -> std::optional<edt::usage_report> {
		return edt::find_usages(*db, nullptr, edt::usage_target{ asset, std::nullopt });
	}, [this] { return db->revision(); });
	const edt::usage_report* first = list.report_for(sky);
	BOOST_REQUIRE(first);
	BOOST_TEST(first->items.size() == 5u);
	BOOST_TEST(list.report_for(sky) == first);
	BOOST_TEST(list.queries() == 1u);
	BOOST_TEST(list.report_for(res::tag::make("materials/child.desc")) != nullptr);
	BOOST_TEST(list.queries() == 2u);

	const edt::asset_usage* inherited = at(*list.report_for(sky), "res://materials/child.desc", "/__parent");
	BOOST_REQUIRE(inherited);
	BOOST_TEST(edt::usage_row_text(*inherited) == "res://materials/child.desc  /__parent  (parent)");
	BOOST_TEST(list.queries() == 3u);

	write("materials/child.desc", R"({ "__type": "material_desc" })");
	res::file_change change;
	change.tag = res::tag::make("materials/child.desc");
	change.kind = res::change_kind::modified;
	assets.apply_changes(std::vector<res::file_change>{ change });
	test_util::pump(tasks);
	const edt::usage_report* after = list.report_for(sky);
	BOOST_REQUIRE(after);
	BOOST_TEST(list.queries() == 4u, "the database moved: asked again");
	BOOST_TEST(after->items.size() == 4u);

	list.set_source([](const res::tag&) -> std::optional<edt::usage_report> { return std::nullopt; }, {});
	BOOST_TEST(list.report_for(sky) == nullptr, "no database: no report");
}

BOOST_AUTO_TEST_SUITE_END()
