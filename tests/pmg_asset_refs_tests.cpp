#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

namespace refs_test {

// A map of tags: a map field needs a C++ type.
class reference_table : public desc::schema_desc<reference_table>
{
public:
	static constexpr std::string_view __type = "refs_test.table";
	std::map<std::string, res::tag> named;
	BOOST_DESCRIBE_CLASS(reference_table, (), (named), (), ())
};

// A registered type without a schema: a writer does not look into it.
struct schema_less : desc::desc_base
{
	void copy_to(desc::desc_base&) const override {}
	void deserialize(desc::desc_system&, const json::object&) override {}
	void serialize(json::object&) const override {}
};

}

// EPIC-57's step assets.refs (#776) in the migration of EPIC-58, on copies of the fixture of old versions (#750).
namespace {

constexpr std::string_view k_with_meta_id = "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53";
constexpr std::string_view k_unknown_id = "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6";
constexpr std::string_view k_duplicate_id = "00000000-0000-4000-8000-0000000000d1";

// Every place a reference stands in, and those a writer leaves alone: "__parent", a custom field, a type without a
// schema, another protocol, a file without an ID, a duplicated ID and an unknown guid.
constexpr std::string_view k_a_desc = R"({
	"__type": "refs_test.thing",
	"__parent": "res://textures/with_meta.png",
	"texture": "res://textures/with_meta.png",
	"material": "res://refs/b.desc",
	"list": [ "res://textures/plain.png", "res://textures/none.png", "memory://t/with_meta.png", "res://textures/dup_a.png" ],
	"table": { "__type": "refs_test.table", "named": {
		"a": "res://textures/with_meta.png",
		"b": { "guid": "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53", "path": "res://textures/old_place.png" },
		"c": { "guid": "0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6", "path": "res://textures/gone.png" } } },
	"slot": { "image": "res://textures/plain.png" },
	"slots": [ { "image": "res://textures/with_meta.png" } ],
	"blob": { "image": "res://textures/with_meta.png" },
	"other": { "__type": "refs_test.schema_less", "texture": "res://textures/with_meta.png" }
})";

constexpr std::string_view k_b_desc = R"({
	"__type": "refs_test.thing",
	"texture": "res://textures/with_meta.png"
})";

// The references of a.desc and what each names before the migration.
const std::vector<std::pair<std::string, std::string>> k_targets = {
	{ "/texture", "res://textures/with_meta.png" },
	{ "/material", "res://refs/b.desc" },
	{ "/list/0", "res://textures/plain.png" },
	{ "/list/1", "res://textures/none.png" },
	{ "/list/2", "memory://t/with_meta.png" },
	{ "/list/3", "res://textures/dup_a.png" },
	{ "/table/named/a", "res://textures/with_meta.png" },
	{ "/table/named/b", "res://textures/with_meta.png" },
	{ "/table/named/c", "res://textures/gone.png" },
	{ "/slot/image", "res://textures/plain.png" },
	{ "/slots/0/image", "res://textures/with_meta.png" },
};

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, std::string_view text)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << text;
}

std::map<std::string, std::string> content_of(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		const std::string relative = fs::relative(entry.path(), root).generic_string();
		if (relative.starts_with(".snk/")) continue;
		files[relative] = read_text(entry.path());
	}
	return files;
}

std::string id_in(const fs::path& sidecar)
{
	const res::asset_meta_result meta = res::read_asset_meta(read_text(sidecar));
	return meta ? res::to_text(meta.meta->guid) : std::string{ "unreadable" };
}

bool any_note(const pmg::migration_report& report, std::string_view part)
{
	return std::ranges::any_of(report.project_notes, [part](const std::string& note) { return note.find(part) != std::string::npos; });
}

json::value reference(std::string_view guid, std::string_view path)
{
	return json::value(json::object{ { "guid", guid }, { "path", path } });
}

// What a reference names: the file that holds its guid now, else its path.
std::string target_of(const json::value& value, const pmg::project_assets& assets)
{
	if (value.is_string()) return std::string(value.as_string());
	const auto read = res::reference_from_json(value);
	if (!read) return "not a reference";
	if (!read->guid.is_nil()) {
		if (const auto now = assets.path_of(read->guid)) return std::string(now->view());
	}
	return std::string(read->path.view());
}

void fails(pmg::project_step_context&)
{
	throw std::runtime_error("injected step failure");
}

// Path <-> ID both ways, as the editor's asset database answers.
struct ids_index final : res::asset_index
{
	std::map<std::string, res::asset_id, std::less<>> ids;
	std::map<res::asset_id, res::tag> paths;

	void hold(const res::asset_id& id, std::string_view path)
	{
		ids.emplace(std::string(path), id);
		paths.insert_or_assign(id, res::tag{ std::string(path) });
	}
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		const auto found = ids.find(tag.view());
		return found == ids.end() ? std::nullopt : std::optional{ found->second };
	}
	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		const auto found = paths.find(id);
		return found == paths.end() ? std::nullopt : std::optional{ found->second };
	}
};

// A folder outside the project, removed with the object: a read-only layer of res:// such as the engine's core/res.
struct outside_folder
{
	outside_folder()
	{
		static std::atomic<int> counter{ 0 };
		path = fs::temp_directory_path()
			/ std::format("snk_refs_layer_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		fs::create_directories(path);
	}
	~outside_folder()
	{
		std::error_code ignored;
		fs::remove_all(path, ignored);
	}
	outside_folder(const outside_folder&) = delete;
	outside_folder& operator=(const outside_folder&) = delete;

	fs::path path;
};

struct asset_refs_fixture : test_util::scene_env
{
	asset_refs_fixture()
	{
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		pmg::register_project_steps(registry);
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
		register_reference_types();
	}

	void register_reference_types()
	{
		descs->register_desc<refs_test::reference_table>("refs_test.table");
		descs->register_desc<refs_test::schema_less>("refs_test.schema_less");
		BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({
			"name": "refs_test.slot", "kind": "struct",
			"fields": [ { "name": "image", "kind": "res_tag", "default": "" } ]
		})").as_object())));
		desc::schema_registry nested;
		nested.add(*descs->find_schema("refs_test.slot"));
		BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({
			"name": "refs_test.thing", "version": 1,
			"fields": [
				{ "name": "texture", "kind": "res_tag", "default": "" },
				{ "name": "material", "kind": "desc_ref", "default": "" },
				{ "name": "list", "kind": "array", "element": "res_tag" },
				{ "name": "slot", "kind": "structure", "ref_type": "refs_test.slot", "default": { "image": "" } },
				{ "name": "slots", "kind": "array", "element": "structure", "ref_type": "refs_test.slot" },
				{ "name": "blob", "kind": "custom" }
			]
		})").as_object(), &nested)));
	}

	// a.desc, b.desc and two textures that carry one ID; dup_b.png is the older, so it holds the ID (res::holds_id_before)
	// and dup_a.png is the copy.
	void write_reference_files()
	{
		write_text(copy.res() / "refs/a.desc", k_a_desc);
		write_text(copy.res() / "refs/b.desc", k_b_desc);
		res::asset_meta duplicate;
		duplicate.guid = *res::asset_id_from_text(k_duplicate_id);
		for (const std::string_view name : { "textures/dup_a.png", "textures/dup_b.png" }) {
			write_text(copy.res() / name, "png");
			write_text(copy.res() / (std::string(name) + ".resource"), res::write_asset_meta(duplicate));
		}
		fs::last_write_time(copy.res() / "textures/dup_a.png", fs::last_write_time(copy.res() / "textures/dup_b.png") + std::chrono::hours(1));
	}

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		options.scan.read_only_layers = read_only;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	pmg::apply_context context(std::string id = "test-migration")
	{
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry, {}, {} };
		context.make_id = [id] { return id; };
		return context;
	}

	pmg::migration_report migrate(std::string id = "test-migration")
	{
		const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context(std::move(id)));
		BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
		BOOST_TEST_MESSAGE("error: " << outcome.report.error);
		BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
		return outcome.report;
	}

	pmg::project_assets assets() const
	{
		pmg::project_assets index(copy.root(), { "res" }, read_only);
		index.rebuild();
		return index;
	}

	json::value a_desc() const { return json::parse(read_text(copy.res() / "refs/a.desc")); }

	static bool planned(const pmg::migration_plan& plan, std::string_view id)
	{
		return std::ranges::find(plan.project_steps, id, &pmg::planned_project_step::id) != plan.project_steps.end();
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry registry;
	// scan_options::read_only_layers of plan() and assets().
	std::vector<fs::path> read_only;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgAssetRefsTests, asset_refs_fixture)

// Strings in the tag fields become {guid, path} with the target's ID; plain.png and b.desc get theirs from assets.ids in
// the same run. What is no reference of a schema, or has no ID, stays as written.
BOOST_AUTO_TEST_CASE(PathsWithAnId_BecomeGuidAndPath_WhatHasNoneStays)
{
	write_reference_files();
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_REQUIRE(scan.find("res/refs/a.desc") != nullptr);
	BOOST_TEST(scan.find("res/refs/a.desc")->reference_fixes == 7u);
	BOOST_TEST(scan.find("res/refs/b.desc")->reference_fixes == 1u);
	BOOST_TEST(scan.find("res/worlds/w_v2.desc")->reference_fixes == 0u);
	const pmg::migration_plan planned_before = plan();
	BOOST_REQUIRE(planned(planned_before, "assets.refs"));

	const pmg::migration_report report = migrate();
	const std::string plain = id_in(copy.res() / "textures/plain.png.resource");
	const std::string b = id_in(copy.res() / "refs/b.desc.resource");
	BOOST_REQUIRE(plain != "unreadable");
	BOOST_REQUIRE(b != "unreadable");

	const json::value after = a_desc();
	const json::value before = json::parse(k_a_desc);
	const auto at = [&after](std::string_view pointer) { return *desc::json_pointer_get(after, pointer); };
	const auto was = [&before](std::string_view pointer) { return *desc::json_pointer_get(before, pointer); };
	BOOST_TEST(at("/texture") == reference(k_with_meta_id, "res://textures/with_meta.png"));
	BOOST_TEST(at("/material") == reference(b, "res://refs/b.desc"), "a desc_ref, to a desc that got its ID in this run");
	BOOST_TEST(at("/list/0") == reference(plain, "res://textures/plain.png"), "plain.png got its ID in this run");
	BOOST_TEST(at("/table/named/a") == reference(k_with_meta_id, "res://textures/with_meta.png"), "a map");
	BOOST_TEST(at("/slot/image") == reference(plain, "res://textures/plain.png"), "a structure");
	BOOST_TEST(at("/slots/0/image") == reference(k_with_meta_id, "res://textures/with_meta.png"), "a list of structures");
	BOOST_TEST(at("/table/named/b") == reference(k_with_meta_id, "res://textures/with_meta.png"), "a stale path is repaired");

	for (const std::string_view kept : { "/__parent", "/list/1", "/list/2", "/list/3", "/table/named/c", "/blob", "/other" }) {
		BOOST_TEST(at(kept) == was(kept), kept);
	}
	BOOST_TEST(json::parse(read_text(copy.res() / "refs/b.desc")).as_object().at("texture") == reference(k_with_meta_id, "res://textures/with_meta.png"));

	BOOST_TEST(any_note(report, "assets.refs: res/refs/a.desc: 6 references turned into {guid, path}, 1 paths repaired"));
	BOOST_TEST(any_note(report, "assets.refs: res/refs/b.desc: 1 references turned into {guid, path}, 0 paths repaired"));
	BOOST_TEST(any_note(report, "assets.refs: 7 references turned into {guid, path}, 1 paths repaired, in 2 desc files"));
}

// AC "no lost references": every reference names the file it named before.
BOOST_AUTO_TEST_CASE(EveryReference_NamesTheSameFileAfterTheMigration)
{
	write_reference_files();
	const json::value before = a_desc();
	const pmg::project_assets old_index = assets();
	for (const auto& [pointer, target] : k_targets) {
		BOOST_REQUIRE_MESSAGE(target_of(*desc::json_pointer_get(before, pointer), old_index) == target, pointer);
	}
	migrate();
	const json::value after = a_desc();
	const pmg::project_assets new_index = assets();
	for (const auto& [pointer, target] : k_targets) {
		BOOST_TEST(target_of(*desc::json_pointer_get(after, pointer), new_index) == target, pointer);
	}
}

// AC "a second run changes nothing": the step is not needed again and nothing is written.
BOOST_AUTO_TEST_CASE(ASecondRun_HasNothingToDo)
{
	write_reference_files();
	migrate();
	const auto migrated = content_of(copy.root());
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_TEST(std::ranges::all_of(scan.files, [](const pmg::scanned_file& file) { return file.reference_fixes == 0; }));
	BOOST_TEST(plan().empty());
	BOOST_TEST((content_of(copy.root()) == migrated));
}

// A type migration that moves a reference into a tag field: the scan reads the old file by the current schema and cannot
// count it, so the step is planned for the outdated file that holds a reference, and one run leaves nothing to do.
BOOST_AUTO_TEST_CASE(AReferenceATypeMigrationMovesIntoATagField_IsWrittenInTheSameRun)
{
	descs->register_migration("refs_test.moved", 1, [](json::object& object) {
		if (auto old = object.find("old_texture"); old != object.end()) {
			json::value moved = std::move(old->value());
			object.erase(old);
			object["texture"] = std::move(moved);
		}
	});
	BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({
		"name": "refs_test.moved", "version": 2,
		"fields": [ { "name": "texture", "kind": "res_tag", "default": "" } ]
	})").as_object())));
	write_text(copy.res() / "refs/moved.desc", R"({"__type":"refs_test.moved","old_texture":"res://textures/with_meta.png"})");

	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	const pmg::scanned_file* moved = scan.find("res/refs/moved.desc");
	BOOST_REQUIRE(moved != nullptr);
	BOOST_TEST(moved->reference_fixes == 0u);
	BOOST_TEST(moved->outdated_references);
	BOOST_TEST(!scan.find("res/worlds/w_v2.desc")->outdated_references, "a __parent is no reference of a field");
	BOOST_REQUIRE(planned(plan(), "assets.refs"));

	migrate();
	const json::value after = json::parse(read_text(copy.res() / "refs/moved.desc"));
	BOOST_TEST(after.as_object().at("texture") == reference(k_with_meta_id, "res://textures/with_meta.png"));
	BOOST_TEST(plan().empty());
}

// "__parent" is no field of a schema: a file whose only reference is one is not written by the step (w_v2, w_v3 stay
// byte for byte as the type migrations left them).
BOOST_AUTO_TEST_CASE(AParentReference_IsLeftAsItIs)
{
	migrate("first");
	const std::string w_v2 = read_text(copy.res() / "worlds/w_v2.desc");
	const std::string w_v3 = read_text(copy.res() / "worlds/w_v3.desc");
	write_reference_files();
	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now, "assets.refs"));
	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context("second"));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(read_text(copy.res() / "worlds/w_v2.desc") == w_v2);
	BOOST_TEST(read_text(copy.res() / "worlds/w_v3.desc") == w_v3);
	BOOST_TEST(!any_note(outcome.report, "res/worlds/w_v"));
}

// AC rollback: a rollback by hand and a failed migration both put the strings back byte for byte.
BOOST_AUTO_TEST_CASE(ARollback_PutsTheStringsBack)
{
	write_reference_files();
	const auto before = content_of(copy.root());
	migrate();
	BOOST_REQUIRE(read_text(copy.res() / "refs/a.desc") != std::string(k_a_desc));
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(AFailedMigration_PutsTheStringsBack)
{
	write_reference_files();
	const auto before = content_of(copy.root());
	pmg::project_step failing;
	failing.id = "test.failing";
	failing.phase = pmg::project_phase::post;
	failing.summary = "fails";
	failing.needed = [](const pmg::scan_result&) { return true; };
	failing.apply = fails;
	registry.add(std::move(failing));
	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now, "assets.refs"));
	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST((content_of(copy.root()) == before));
}

// The index of a migration answers as resource_system::asset_id_of does: the first content root that has the file, no ID
// for a duplicate, the res:// tag of the one file that holds an ID.
BOOST_AUTO_TEST_CASE(TheMigrationIndex_AnswersByTag)
{
	write_reference_files();
	const pmg::project_assets index = assets();
	const res::asset_id with_meta = *res::asset_id_from_text(k_with_meta_id);
	BOOST_TEST((index.id_of(res::tag{ "res://textures/with_meta.png" }) == with_meta));
	BOOST_TEST(!index.id_of(res::tag{ "memory://textures/with_meta.png" }).has_value());
	BOOST_TEST(!index.id_of(res::tag{ "res://textures/plain.png" }).has_value(), "no .resource");
	const res::asset_id duplicate = *res::asset_id_from_text(k_duplicate_id);
	BOOST_TEST(!index.id_of(res::tag{ "res://textures/dup_a.png" }).has_value(), "the newer copy of a duplicated ID");
	BOOST_TEST((index.id_of(res::tag{ "res://textures/dup_b.png" }) == duplicate), "the older file holds it");
	BOOST_REQUIRE(index.path_of(duplicate).has_value());
	BOOST_TEST(index.path_of(duplicate)->view() == "res://textures/dup_b.png");
	BOOST_TEST(index.id_of(res::tag{ "res://Textures/With_Meta.png" }).has_value() == res::asset_names_case_insensitive,
		"another letter case is the same file only where asset names have no case");
	BOOST_TEST(!index.path_of(*res::asset_id_from_text(k_unknown_id)).has_value());
	BOOST_REQUIRE(index.path_of(with_meta).has_value());
	BOOST_TEST(index.path_of(with_meta)->view() == "res://textures/with_meta.png");
}

// The editor's save and the step share one walker: with the resource system's index both write the same tree.
BOOST_AUTO_TEST_CASE(WriteReferencesWith_TheResourceSystemsIndex_IsWriteReferences)
{
	auto index = std::make_shared<ids_index>();
	index->hold(*res::asset_id_from_text(k_with_meta_id), "res://textures/with_meta.png");
	index->hold(*res::asset_id_from_text("11111111-2222-4333-8444-555555555555"), "res://textures/plain.png");
	rs->set_asset_index(index);
	const json::value original = json::parse(k_a_desc);
	json::value saved = original;
	descs->write_references(saved);
	json::value shared = original;
	const desc::reference_counts counts = descs->write_references_with(shared, desc::asset_index_ids(*rs));
	rs->set_asset_index(nullptr);

	BOOST_TEST(saved != original);
	BOOST_TEST(shared == saved);
	BOOST_TEST(counts.ids_written == 5u);
	BOOST_TEST(counts.paths_repaired == 1u);
}

// Of two files with one ID the one res::holds_id_before puts first (here the older) holds it, as in the editor's asset
// index: a reference to it gets {guid, path}, a reference to the copy stays a string.
BOOST_AUTO_TEST_CASE(OfTwoFilesWithOneId_OnlyTheHolderGetsGuidAndPath)
{
	write_reference_files();
	write_text(copy.res() / "refs/dup.desc", R"({"__type": "refs_test.thing", "list": ["res://textures/dup_a.png", "res://textures/dup_b.png"]})");
	migrate();
	const json::value after = json::parse(read_text(copy.res() / "refs/dup.desc"));
	BOOST_TEST(*desc::json_pointer_get(after, "/list/0") == json::value("res://textures/dup_a.png"));
	BOOST_TEST(*desc::json_pointer_get(after, "/list/1") == reference(k_duplicate_id, "res://textures/dup_b.png"));
	BOOST_TEST(plan().empty());
}

// A {"guid"} without a path whose ID is known gets the path its file has now, as write_references gives it.
BOOST_AUTO_TEST_CASE(AGuidWithoutAPath_GetsThePathOfItsFile)
{
	write_text(copy.res() / "refs/g.desc", std::string(R"({"__type": "refs_test.thing", "texture": {"guid": ")") + std::string(k_with_meta_id) + "\"}}");
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_REQUIRE(scan.find("res/refs/g.desc") != nullptr);
	BOOST_TEST(scan.find("res/refs/g.desc")->reference_fixes == 1u);
	const pmg::migration_report report = migrate();
	const json::value after = json::parse(read_text(copy.res() / "refs/g.desc"));
	BOOST_TEST(after.as_object().at("texture") == reference(k_with_meta_id, "res://textures/with_meta.png"));
	BOOST_TEST(any_note(report, "assets.refs: res/refs/g.desc: 0 references turned into {guid, path}, 1 paths repaired"));
	BOOST_TEST(plan().empty());
}

// A reference to an asset of a read-only layer below the project (the engine's core/res) gets the ID of that asset's
// .resource, as in the editor, whose asset database walks every layer; nothing is written into that layer.
BOOST_AUTO_TEST_CASE(AReferenceToAnAssetOfAReadOnlyLayer_GetsItsId)
{
	outside_folder engine;
	constexpr std::string_view engine_id = "00000000-0000-4000-8000-0000000000e1";
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(engine_id);
	write_text(engine.path / "shaders/lit.png", "png");
	write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
	write_text(engine.path / "shaders/plain.png", "png");
	read_only = { engine.path };
	write_text(copy.res() / "refs/e.desc", R"({"__type": "refs_test.thing", "texture": "res://shaders/lit.png", "list": ["res://shaders/plain.png"]})");

	migrate();
	const json::value after = json::parse(read_text(copy.res() / "refs/e.desc"));
	BOOST_TEST(after.as_object().at("texture") == reference(engine_id, "res://shaders/lit.png"));
	BOOST_TEST(*desc::json_pointer_get(after, "/list/0") == json::value("res://shaders/plain.png"), "a file of that layer without an ID");
	BOOST_TEST(!fs::exists(engine.path / "shaders/plain.png.resource"), "assets.ids writes nothing into a read-only layer");
	BOOST_TEST(plan().empty());
}

// #902: a project file whose .resource carries the ID of an engine file (a copied asset with its .resource). The
// engine's file keeps the ID, so the copy gets a new one in the same run, and a path to the copy becomes {guid, path}
// with it.
struct engine_copy
{
	static constexpr std::string_view engine_id = "00000000-0000-4000-8000-0000000000e2";

	explicit engine_copy(asset_refs_fixture& fixture) : fixture(fixture)
	{
		res::asset_meta meta;
		meta.guid = *res::asset_id_from_text(engine_id);
		write_text(engine.path / "shaders/lit.png", "png");
		write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
		meta.importer = "test";
		meta.flags = { "editor_only" };
		write_text(fixture.copy.res() / "textures/lit_copy.png", "png");
		write_text(fixture.copy.res() / "textures/lit_copy.png.resource", res::write_asset_meta(meta));
		// The guid of the engine's file in an object: a reference to that file, which stays one.
		write_text(fixture.copy.res() / "refs/e.desc", std::format(R"({{"__type": "refs_test.thing",
			"texture": "res://textures/lit_copy.png",
			"list": [ {{ "guid": "{}", "path": "res://textures/lit_copy.png" }},
				"res://shaders/lit.png" ]}})", engine_id));
		fixture.read_only = { engine.path };
	}

	asset_refs_fixture& fixture;
	outside_folder engine;
};

BOOST_AUTO_TEST_CASE(ACopyOfAnEngineFilesId_GetsANewId_AndPathsToItBecomeReferences)
{
	engine_copy setup(*this);
	const std::string engine_meta = read_text(setup.engine.path / "shaders/lit.png.resource");

	const pmg::migration_plan planned_now = plan();
	const pmg::scanned_file* copied = planned_now.scan.find("res/textures/lit_copy.png");
	BOOST_REQUIRE(copied != nullptr);
	BOOST_TEST(copied->id_held_below == "res://shaders/lit.png");
	BOOST_TEST(copied->id_held_below_meta == "res/textures/lit_copy.png.resource");
	BOOST_TEST(planned(planned_now, "assets.ids_below"));
	BOOST_TEST(planned(planned_now, "assets.refs"));
	const pmg::scanned_file* referring = planned_now.scan.find("res/refs/e.desc");
	BOOST_REQUIRE(referring != nullptr);
	BOOST_TEST((referring->references_to_copies == std::vector<std::string>{ "res://textures/lit_copy.png" }),
		"only the path: the {guid, path} keeps the engine's guid");

	const pmg::migration_report report = migrate();
	const std::string new_id = id_in(copy.res() / "textures/lit_copy.png.resource");
	BOOST_TEST(new_id != std::string(engine_copy::engine_id));
	const res::asset_meta_result kept = res::read_asset_meta(read_text(copy.res() / "textures/lit_copy.png.resource"));
	BOOST_REQUIRE(kept);
	BOOST_TEST(kept.meta->importer == "test");
	BOOST_TEST((kept.meta->flags == std::vector<std::string>{ "editor_only" }));
	BOOST_TEST(read_text(setup.engine.path / "shaders/lit.png.resource") == engine_meta,
		"nothing is written below the project");

	const json::value after = json::parse(read_text(copy.res() / "refs/e.desc"));
	BOOST_TEST(after.as_object().at("texture") == reference(new_id, "res://textures/lit_copy.png"));
	const auto kept_reference = res::reference_from_json(*desc::json_pointer_get(after, "/list/0"));
	BOOST_REQUIRE(kept_reference);
	BOOST_TEST(res::to_text(kept_reference->guid) == std::string(engine_copy::engine_id),
		"a reference to the engine's file");
	BOOST_TEST(*desc::json_pointer_get(after, "/list/1") == reference(engine_copy::engine_id, "res://shaders/lit.png"));

	BOOST_TEST(any_note(report, std::format("assets.ids_below: res/textures/lit_copy.png: its ID {} belongs to {}",
		engine_copy::engine_id, "res://shaders/lit.png")));
	BOOST_TEST(any_note(report, new_id));
	BOOST_TEST(any_note(report,
		"assets.refs: res/refs/e.desc: res://textures/lit_copy.png turned into {guid, path} with the new ID"));
	BOOST_TEST(plan().empty());
}

BOOST_AUTO_TEST_CASE(TheDryRunOfACopyOfAnEngineFilesId_ListsItAndWritesNothing)
{
	engine_copy setup(*this);
	const auto before = content_of(copy.root());
	const std::string text = pmg::plan_text(plan());
	BOOST_TEST(text.find("step assets.ids_below (pre)") != std::string::npos, text);
	BOOST_TEST(text.find("- res/textures/lit_copy.png (res/textures/lit_copy.png.resource): ID of res://shaders/lit.png")
		!= std::string::npos, text);
	BOOST_TEST(text.find("- res/refs/e.desc: res://textures/lit_copy.png") != std::string::npos, text);
	BOOST_TEST((content_of(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(ARollback_PutsBackTheResourceOfTheCopyAndItsReferences)
{
	engine_copy setup(*this);
	const auto before = content_of(copy.root());
	migrate();
	BOOST_REQUIRE(id_in(copy.res() / "textures/lit_copy.png.resource") != std::string(engine_copy::engine_id));
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

// A project file at the path of an engine file shadows it, as res:// does: the walk lists only the project's, so it
// holds the ID itself and is no copy; a path to it takes that ID.
BOOST_AUTO_TEST_CASE(AProjectFileAtThePathOfAnEngineFile_HoldsItsIdItself)
{
	outside_folder engine;
	constexpr std::string_view engine_id = "00000000-0000-4000-8000-0000000000e4";
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(engine_id);
	write_text(engine.path / "shaders/lit.png", "png");
	write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "shaders/lit.png", "project png");
	write_text(copy.res() / "shaders/lit.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "refs/s.desc", std::format(R"({{"__type": "refs_test.thing",
		"texture": "res://shaders/lit.png",
		"list": [ {{ "guid": "{}", "path": "res://shaders/lit.png" }} ]}})", engine_id));
	read_only = { engine.path };

	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned_now.scan.find("res/shaders/lit.png") != nullptr);
	BOOST_TEST(planned_now.scan.find("res/shaders/lit.png")->id_held_below.empty());
	BOOST_TEST(!planned(planned_now, "assets.ids_below"));
	BOOST_TEST(pmg::plan_text(planned_now).find("New IDs") == std::string::npos);
	BOOST_TEST(assets().path_of(meta.guid)->view() == "res://shaders/lit.png");
	BOOST_TEST(!assets().held_below(meta.guid));

	migrate();
	BOOST_TEST(id_in(copy.res() / "shaders/lit.png.resource") == std::string(engine_id));
	const json::value after = json::parse(read_text(copy.res() / "refs/s.desc"));
	BOOST_TEST(after.as_object().at("texture") == reference(engine_id, "res://shaders/lit.png"));
	BOOST_TEST(*desc::json_pointer_get(after, "/list/0") == reference(engine_id, "res://shaders/lit.png"));
	BOOST_TEST(plan().empty());
}

// The .resource of a copy may lie in another content root: the step rewrites the one the walk paired with the file.
BOOST_AUTO_TEST_CASE(ACopyWhoseResourceIsInALowerRoot_GetsItsNewIdThere)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "base" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	outside_folder engine;
	constexpr std::string_view engine_id = "00000000-0000-4000-8000-0000000000e5";
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(engine_id);
	write_text(engine.path / "shaders/lit.png", "png");
	write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "textures/up.png", "upper");
	write_text(copy.root() / "base/textures/up.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "refs/up.desc", R"({"__type": "refs_test.thing", "texture": "res://textures/up.png"})");
	read_only = { engine.path };

	const pmg::migration_plan planned_now = plan();
	const pmg::scanned_file* copied = planned_now.scan.find("res/textures/up.png");
	BOOST_REQUIRE(copied != nullptr);
	BOOST_TEST(copied->id_held_below == "res://shaders/lit.png");
	BOOST_TEST(copied->id_held_below_meta == "base/textures/up.png.resource");

	const pmg::migration_report report = migrate();
	const std::string new_id = id_in(copy.root() / "base/textures/up.png.resource");
	BOOST_TEST(new_id != std::string(engine_id));
	BOOST_TEST(!fs::exists(copy.res() / "textures/up.png.resource"));
	BOOST_TEST(json::parse(read_text(copy.res() / "refs/up.desc")).as_object().at("texture")
		== reference(new_id, "res://textures/up.png"));
	BOOST_TEST(any_note(report, "assets.refs: res/refs/up.desc: res://textures/up.png turned into {guid, path}"));
	BOOST_TEST(plan().empty());
}

// assets.ids: a desc whose __guid is the ID of an engine file takes a new ID, not that one.
BOOST_AUTO_TEST_CASE(ADescWhoseGuidIsAnEngineFilesId_GetsANewId)
{
	outside_folder engine;
	constexpr std::string_view engine_id = "00000000-0000-4000-8000-0000000000e3";
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(engine_id);
	write_text(engine.path / "shaders/lit.png", "png");
	write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
	read_only = { engine.path };
	write_text(copy.res() / "refs/g.desc", std::format(R"({{"__type": "refs_test.thing", "__guid": "{}"}})", engine_id));

	const pmg::migration_report report = migrate();
	const std::string id = id_in(copy.res() / "refs/g.desc.resource");
	BOOST_TEST(id != "unreadable");
	BOOST_TEST(id != std::string(engine_id));
	BOOST_TEST(any_note(report, "is already the ID of res://shaders/lit.png (a layer below the project)"));
	BOOST_TEST(plan().empty());
}

// A file of the upper content root whose .resource lies only in the lower one: assets.ids gives it none (it has one) and
// assets.refs takes that ID, so one run is enough.
BOOST_AUTO_TEST_CASE(AFileWhoseResourceIsInALowerRoot_IsReferencedByThatId_TheSecondPlanIsEmpty)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "base" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	constexpr std::string_view lower_id = "00000000-0000-4000-8000-0000000000f1";
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(lower_id);
	write_text(copy.root() / "base/textures/up.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "textures/up.png", "upper");
	write_text(copy.res() / "refs/up.desc", R"({"__type": "refs_test.thing", "texture": "res://textures/up.png"})");

	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_REQUIRE(scan.find("res/textures/up.png") != nullptr);
	BOOST_TEST(!pmg::gets_resource_from_ids(*scan.find("res/textures/up.png")));
	BOOST_TEST(scan.find("res/refs/up.desc")->reference_fixes == 1u);

	migrate();
	BOOST_TEST(!fs::exists(copy.res() / "textures/up.png.resource"));
	BOOST_TEST(json::parse(read_text(copy.res() / "refs/up.desc")).as_object().at("texture") == reference(lower_id, "res://textures/up.png"));
	BOOST_TEST(plan().empty());
}

// The goal of the step: after the migration a save in the editor (write_references_over the file itself, with an index
// that answers as the migration's) changes nothing.
BOOST_AUTO_TEST_CASE(AfterTheMigration_AnEditorSave_ChangesNoReference)
{
	write_reference_files();
	migrate();
	const pmg::project_assets migrated = assets();
	auto index = std::make_shared<ids_index>();
	for (const auto& [path, asset] : migrated.assets()) {
		if (!asset.id) continue;
		if (const auto holder = migrated.path_of(*asset.id); holder && holder->view() == "res://" + asset.res_path) {
			index->hold(*asset.id, holder->view());
		}
	}
	rs->set_asset_index(index);
	for (const std::string_view file : { "refs/a.desc", "refs/b.desc" }) {
		const json::value written = json::parse(read_text(copy.res() / file));
		json::value saved = written;
		descs->write_references_over(saved, res::tag::make(file));
		BOOST_TEST(saved == written, file);
	}
	rs->set_asset_index(nullptr);
}

// The one difference of the editor's save: a guid the replaced file wrote for a path the index gives no ID is kept
// (write_references_over); the migration has no replaced file, and a {guid, path} it reads stays as it is anyway.
BOOST_AUTO_TEST_CASE(WriteReferencesOver_KeepsAGuidOfTheReplacedFile_WriteReferencesWithDoesNot)
{
	auto index = std::make_shared<ids_index>();
	index->hold(*res::asset_id_from_text(k_with_meta_id), "res://textures/with_meta.png");
	rs->set_asset_index(index);
	write_text(copy.res() / "refs/kept.desc", std::string(R"({"__type": "refs_test.thing", "texture": {"guid": ")") + std::string(k_unknown_id)
		+ R"(", "path": "res://textures/none.png"}, "list": ["res://textures/with_meta.png"]})");
	const json::value edited = json::parse(R"({"__type": "refs_test.thing", "texture": "res://textures/none.png", "list": ["res://textures/with_meta.png"]})");
	json::value over = edited;
	descs->write_references_over(over, res::tag::make("refs/kept.desc"));
	json::value with = edited;
	descs->write_references_with(with, desc::asset_index_ids(*rs));
	rs->set_asset_index(nullptr);

	BOOST_TEST(over.as_object().at("texture") == reference(k_unknown_id, "res://textures/none.png"), "the replaced file's guid is kept");
	BOOST_TEST(with.as_object().at("texture") == json::value("res://textures/none.png"));
	BOOST_TEST(*desc::json_pointer_get(over, "/list/0") == *desc::json_pointer_get(with, "/list/0"), "where the index answers, both agree");
	BOOST_TEST(*desc::json_pointer_get(with, "/list/0") == reference(k_with_meta_id, "res://textures/with_meta.png"));
}

// The read-only layers are the layers of res:// outside the project folder, however the project root is spelled; a
// content root is never one of them.
BOOST_AUTO_TEST_CASE(ReadOnlyLayers_AreTheLayersOfResOutsideTheProject)
{
	outside_folder engine;
	BOOST_REQUIRE(rs->remount(res::tag::default_protocol(), { engine.path, copy.res() }));
	BOOST_TEST((pmg::read_only_layers(*rs, copy.root()) == std::vector<fs::path>{ engine.path }));
	BOOST_TEST((pmg::read_only_layers(*rs, copy.root() / "res" / "..") == std::vector<fs::path>{ engine.path }));
	BOOST_TEST((pmg::read_only_layers(*rs, copy.root() / "res") == std::vector<fs::path>{ engine.path }));
}

// The scan lists every .resource of the read-only layers, which the plan's fingerprint stats.
BOOST_AUTO_TEST_CASE(TheScan_ListsTheResourcesOfTheReadOnlyLayers)
{
	outside_folder engine;
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text("00000000-0000-4000-8000-0000000000e1");
	write_text(engine.path / "shaders/lit.png", "png");
	write_text(engine.path / "shaders/lit.png.resource", res::write_asset_meta(meta));
	write_text(engine.path / "shaders/plain.png", "png");
	pmg::scan_options options;
	options.read_only_layers = { engine.path };
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs, options);
	BOOST_REQUIRE(scan.read_only_resources.size() == 1u);
	BOOST_TEST((scan.read_only_resources.front().layer == engine.path));
	BOOST_TEST(scan.read_only_resources.front().path == "shaders/lit.png.resource");
	BOOST_TEST(scan.find("shaders/lit.png") == nullptr, "a read-only layer is not scanned");
}

BOOST_AUTO_TEST_SUITE_END()
