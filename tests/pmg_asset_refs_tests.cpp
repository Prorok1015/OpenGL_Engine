#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

	// a.desc, b.desc and two textures that carry one ID.
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
	}

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
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
		pmg::project_assets index(copy.root(), { "res" });
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
	BOOST_TEST(!index.id_of(res::tag{ "res://textures/dup_a.png" }).has_value(), "a duplicated ID");
	BOOST_TEST(!index.path_of(*res::asset_id_from_text(k_duplicate_id)).has_value());
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

BOOST_AUTO_TEST_SUITE_END()
