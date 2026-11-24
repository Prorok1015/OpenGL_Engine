#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/desc/desc_schema.h>
#include <snk/model_codec/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// The project step engine.material_shaders (#1200) on copies of the fixture of old versions (#750): material stages the
// type step refuses get a .shader.desc before the type migrations, so the project migrates instead of failing.
namespace {

constexpr std::string_view k_step = "engine.material_shaders";

constexpr std::string_view k_custom_pair = R"({
	"__type": "material_desc",
	"__version": 1,
	"shader_vertex": "res://shaders/custom.vert",
	"shader_fragment": "res://shaders/custom.frag",
	"defines": ["USE_FOG"]
})";

// A shader_desc that is no output of the step: what stands at a name it must not take.
constexpr std::string_view k_foreign_shader = R"({
	"__type": "shader_desc",
	"__version": 1,
	"vertex": "res://shaders/foreign.vert",
	"fragment": "res://shaders/foreign.frag",
	"defines": []
})";

// A material of its own pair that holds an object of the test type pmg_shaders_test_desc (register_test_type).
constexpr std::string_view k_with_other_type = R"({
	"__type": "material_desc",
	"__version": 1,
	"shader_vertex": "res://shaders/other.vert",
	"shader_fragment": "res://shaders/other.frag",
	"extra": { "__type": "pmg_shaders_test_desc", "hp": 1 }
})";

constexpr std::string_view k_engine_base_guid = "f90d3c03-865c-4193-aa09-cb2f98315634";

// A folder outside the project, removed with the object: a read-only layer of res:// such as the engine's core/res.
struct outside_folder
{
	outside_folder()
	{
		static std::atomic<int> counter{ 0 };
		path = fs::temp_directory_path()
			/ std::format("snk_shaders_layer_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
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

bool any_note(const pmg::migration_report& report, std::string_view part)
{
	return std::ranges::any_of(report.project_notes, [part](const std::string& note) { return note.find(part) != std::string::npos; });
}

bool planned(const pmg::migration_plan& plan)
{
	return std::ranges::find(plan.project_steps, k_step, &pmg::planned_project_step::id) != plan.project_steps.end();
}

std::vector<std::string> created_shaders(const pmg::migration_report& report)
{
	std::vector<std::string> out;
	for (const std::string& path : report.created_files) {
		if (path.ends_with(".shader.desc")) out.push_back(path);
	}
	std::ranges::sort(out);
	return out;
}

struct shaders_fixture : test_util::scene_env
{
	shaders_fixture()
	{
		// The fixture without its newer and broken files, which a migration refuses.
		for (const test_util::fixture_copy* project : { &copy, &other }) {
			fs::remove(project->res() / "newer.desc");
			fs::remove(project->res() / "broken.desc");
		}
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		engine::render::register_render_descs(*descs);
		// The module of res/module/unknown.desc, loaded: the migration is complete.
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
		pmg::register_engine_project_steps(registry);
	}

	~shaders_fixture()
	{
		engine::render::unregister_render_descs(*descs);
	}

	pmg::migration_plan plan(const fs::path& root, bool force = false)
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		options.force = force;
		options.scan.read_only_layers = read_only;
		return pmg::plan_migration(root, pmg::scan_project(root, *descs, options.scan), *descs, options);
	}

	pmg::migration_plan plan() { return plan(copy.root()); }

	// pmg_shaders_test_desc at version 1, with a step to 2 that does what `step` does.
	void register_test_type(std::function<void(json::object&, desc::migration_context&)> step)
	{
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "pmg_shaders_test_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
		descs->register_migration("pmg_shaders_test_desc", 1, "a test step", std::move(step));
	}

	pmg::apply_context context(const fs::path& root)
	{
		pmg::apply_context context{ root, *descs, *rs, {}, {}, {}, {}, &registry, {}, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	pmg::migration_report migrate(const fs::path& root)
	{
		const pmg::migration_plan planned_now = plan(root);
		BOOST_REQUIRE(planned(planned_now));
		const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context(root));
		BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
		BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
		return outcome.report;
	}

	pmg::migration_report migrate() { return migrate(copy.root()); }

	fs::path at(std::string_view path) const { return copy.root() / std::string(path); }

	json::object read_json(std::string_view path) const { return json::parse(read_text(at(path))).as_object(); }

	// The engine's base material in a read-only layer of res://, with its real ID, as the engine ships it after #1200.
	void serve_engine_base()
	{
		write_text(engine.path / "base_material.desc", R"({
			"__type": "material_desc",
			"__version": 2,
			"shader": "res://shaders/scene.shader.desc",
			"queue": "opaque"
		})");
		res::asset_meta meta;
		meta.guid = *res::asset_id_from_text(k_engine_base_guid);
		write_text(engine.path / "base_material.desc.resource", res::write_asset_meta(meta));
		write_text(engine.path / "shaders/scene.shader.desc", R"({
			"__type": "shader_desc",
			"__version": 1,
			"vertex": "res://shaders/scene.vert",
			"fragment": "res://shaders/scene.frag",
			"defines": []
		})");
		BOOST_REQUIRE(rs->remount(res::tag::default_protocol(), { engine.path, copy.res() }));
		read_only = { engine.path };
	}

	test_util::fixture_copy copy{ "migration_v1" };
	// A second project for the runs that are compared with the first.
	test_util::fixture_copy other{ "migration_v1" };
	pmg::project_step_registry registry;
	outside_folder engine;
	// scan_options::read_only_layers of plan().
	std::vector<fs::path> read_only;
};

const json::object* find_material(const json::value& value)
{
	if (const json::object* object = value.if_object()) {
		if (const json::value* type = object->if_contains("__type"); type && type->is_string() && type->as_string() == "material_desc") return object;
		for (const auto& member : *object) {
			if (const json::object* found = find_material(member.value())) return found;
		}
	} else if (const json::array* array = value.if_array()) {
		for (const json::value& item : *array) {
			if (const json::object* found = find_material(item)) return found;
		}
	}
	return nullptr;
}

void check_material_v2(const json::object& material, std::string_view shader)
{
	BOOST_TEST(material.at("__version").as_int64() == 2);
	BOOST_TEST(material.at("shader").as_string() == shader);
	for (const char* key : { "shader_vertex", "shader_fragment", "shader_geometry" }) {
		BOOST_TEST(!material.contains(key));
	}
}

}

BOOST_FIXTURE_TEST_SUITE(PmgMaterialShadersTests, shaders_fixture)

BOOST_AUTO_TEST_CASE(AnUnknownPair_GetsAShaderDescBesideTheMaterial_AndTheMaterialBecomesV2)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/rock.shader.desc" }));
	BOOST_TEST(fs::exists(at("res/materials/rock.shader.desc.resource")));
	BOOST_TEST(any_note(report, "res/materials/rock.shader.desc: created, referenced by res/materials/rock.desc"));
	BOOST_TEST(any_note(report, "1 materials, 1 shader files, 0 not migrated"));

	const json::object shader = read_json("res/materials/rock.shader.desc");
	BOOST_TEST(shader.at("__type").as_string() == "shader_desc");
	BOOST_TEST(shader.at("vertex").as_string() == "res://shaders/custom.vert");
	BOOST_TEST(shader.at("fragment").as_string() == "res://shaders/custom.frag");
	BOOST_TEST(!shader.contains("geometry"));

	const json::object material = read_json("res/materials/rock.desc");
	check_material_v2(material, "res://materials/rock.shader.desc");
	BOOST_TEST(material.at("defines").as_array().size() == 1u);

	// Done once: a second plan does not need the step.
	BOOST_TEST(!planned(plan()));
}

BOOST_AUTO_TEST_CASE(TwoMaterialsOfOnePair_ShareOneFile)
{
	write_text(at("res/materials/a.desc"), k_custom_pair);
	write_text(at("res/materials/b.desc"), k_custom_pair);

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/a.shader.desc" }));
	BOOST_TEST(any_note(report, "res/materials/a.shader.desc: created, referenced by res/materials/a.desc, res/materials/b.desc"));
	check_material_v2(read_json("res/materials/a.desc"), "res://materials/a.shader.desc");
	check_material_v2(read_json("res/materials/b.desc"), "res://materials/a.shader.desc");
	BOOST_TEST(!fs::exists(at("res/materials/b.shader.desc")));
}

// The child sets only its fragment: its vertex comes from a project parent between it and the engine's base.
BOOST_AUTO_TEST_CASE(AFragmentOverAProjectParent_TakesTheParentsVertex)
{
	write_text(at("res/materials/base.desc"), k_custom_pair);
	write_text(at("res/materials/child.desc"), R"({
		"__type": "material_desc",
		"__parent": "res://materials/base.desc",
		"shader_fragment": "res://shaders/glow.frag"
	})");

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/base.shader.desc", "res/materials/child.shader.desc" }));
	const json::object shader = read_json("res/materials/child.shader.desc");
	BOOST_TEST(shader.at("vertex").as_string() == "res://shaders/custom.vert");
	BOOST_TEST(shader.at("fragment").as_string() == "res://shaders/glow.frag");
	const json::object child = read_json("res/materials/child.desc");
	check_material_v2(child, "res://materials/child.shader.desc");
	BOOST_TEST(child.at("__parent").as_string() == "res://materials/base.desc");
}

// A project file at res://base_material.desc shadows the engine's: the type step does not take the engine's vertex for
// granted, the project step reads the parent and the triple is an engine pair, so the engine's file is referenced.
BOOST_AUTO_TEST_CASE(AnEnginePairUnderAShadowingProjectBase_ReferencesTheEnginesFile)
{
	write_text(at("res/base_material.desc"), R"({
		"__type": "material_desc",
		"__version": 1,
		"shader_vertex": "res://shaders/scene.vert",
		"shader_fragment": "res://shaders/scene.frag"
	})");
	write_text(at("res/materials/glass.desc"), R"({
		"__type": "material_desc",
		"__parent": "res://base_material.desc",
		"shader_fragment": "res://shaders/mix_opaque_trans_scene.frag"
	})");

	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now));
	BOOST_TEST(planned_now.scan.find("res/materials/glass.desc")->material_stages);
	BOOST_TEST(!planned_now.scan.find("res/base_material.desc")->material_stages);

	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_TEST(created_shaders(outcome.report).empty());
	BOOST_TEST(any_note(outcome.report, "res://shaders/mix_opaque_trans_scene.shader.desc: the engine's shader, now referenced by res/materials/glass.desc"));
	check_material_v2(read_json("res/materials/glass.desc"), "res://shaders/mix_opaque_trans_scene.shader.desc");
	check_material_v2(read_json("res/base_material.desc"), "res://shaders/scene.shader.desc");
}

// The dry run: the step is planned before the type migrations, the material is planned (not invalid) and nothing is written.
BOOST_AUTO_TEST_CASE(TheDryRun_ListsTheStep_AndNoFailure_AndWritesNothing)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	const auto before = content_of(copy.root());

	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now));
	const auto step = std::ranges::find(planned_now.project_steps, k_step, &pmg::planned_project_step::id);
	BOOST_TEST(step->phase == "pre");
	BOOST_TEST(pmg::plan_refusal(planned_now).empty(), pmg::plan_refusal(planned_now));
	const pmg::scanned_file* file = planned_now.scan.find("res/materials/rock.desc");
	BOOST_REQUIRE(file);
	BOOST_TEST((file->kind == pmg::file_class::outdated));
	BOOST_TEST(file->material_stages);
	BOOST_TEST((std::ranges::find(planned_now.files, std::string("res/materials/rock.desc"), &pmg::file_plan::path) != planned_now.files.end()));
	BOOST_TEST(pmg::skipped_by(planned_now).empty());
	BOOST_TEST((content_of(copy.root()) == before));
}

// A name that is taken is never overwritten: the file gets -2.
BOOST_AUTO_TEST_CASE(ATakenName_GetsADashSuffix_AndTheFileThereStays)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	write_text(at("res/materials/rock.shader.desc"), k_foreign_shader);

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/rock-2.shader.desc" }));
	BOOST_TEST(read_text(at("res/materials/rock.shader.desc")) == k_foreign_shader);
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock-2.shader.desc");
}

// The name is unique by its res:// path: a file of another content root at that path would shadow the new file (or be
// shadowed by it), so the new one gets -2 too.
BOOST_AUTO_TEST_CASE(ANameTakenInAnotherContentRoot_GetsADashSuffix)
{
	json::object manifest = json::parse(read_text(at("project.snkproj"))).as_object();
	manifest["content_roots"] = json::array{ "res", "extra" };
	write_text(at("project.snkproj"), json::serialize(manifest));
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	write_text(at("extra/materials/rock.shader.desc"), k_foreign_shader);

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/rock-2.shader.desc" }));
	BOOST_TEST(read_text(at("extra/materials/rock.shader.desc")) == k_foreign_shader);
	BOOST_TEST(!fs::exists(at("res/materials/rock.shader.desc")));
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock-2.shader.desc");
}

// The same for a file of a read-only layer (the engine's): the project's file would hide it.
BOOST_AUTO_TEST_CASE(ANameTakenInAReadOnlyLayer_GetsADashSuffix)
{
	serve_engine_base();
	write_text(engine.path / "materials/rock.shader.desc", k_foreign_shader);
	write_text(at("res/materials/rock.desc"), k_custom_pair);

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/rock-2.shader.desc" }));
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock-2.shader.desc");
}

// A material the step could not convert (its parent is missing) is invalid in the plan, as a file whose type step fails:
// the plan is refused without --force, and with it the other material migrates and this one stays as it is.
BOOST_AUTO_TEST_CASE(AMaterialTheStepCannotConvert_IsInvalid_AndForceMigratesTheRest)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	constexpr std::string_view orphan = R"({
		"__type": "material_desc",
		"__parent": "res://materials/missing.desc",
		"shader_fragment": "res://shaders/glow.frag"
	})";
	write_text(at("res/materials/orphan.desc"), orphan);

	const pmg::migration_plan strict = plan();
	const pmg::scanned_file* bad = strict.scan.find("res/materials/orphan.desc");
	BOOST_REQUIRE(bad);
	BOOST_TEST((bad->kind == pmg::file_class::invalid));
	BOOST_TEST(!bad->material_stages);
	BOOST_TEST(bad->reason.find("res://materials/missing.desc") != std::string::npos, bad->reason);
	BOOST_TEST(!pmg::plan_refusal(strict).empty());
	BOOST_TEST(strict.scan.find("res/materials/rock.desc")->material_stages);

	const pmg::migration_plan forced = plan(copy.root(), true);
	BOOST_REQUIRE(pmg::plan_refusal(forced).empty());
	const pmg::apply_outcome outcome = pmg::apply_migration(forced, context(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial), outcome.report.error);
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock.shader.desc");
	BOOST_TEST(read_text(at("res/materials/orphan.desc")) == orphan);
}

// A material of the old importer (the engine's base as its parent, a fragment only) is the type step's: --migrate gives
// resource_system the project's index, so the plan's and the apply's type step see the engine's base as the scan does.
BOOST_AUTO_TEST_CASE(AnImportedMaterialOverTheEnginesBase_IsConvertedByTheTypeStep_InTheCommand)
{
	serve_engine_base();
	write_text(at("res/materials/imported.desc"), R"({
		"__type": "material_desc",
		"__parent": "res://base_material.desc",
		"shader_fragment": "res://shaders/mix_opaque_trans_scene.frag"
	})");

	{
		const pmg::scoped_project_index index(*rs, copy.root(), read_only);
		BOOST_REQUIRE(rs->has_asset_index());
		const pmg::migration_plan planned_now = plan();
		const pmg::scanned_file* file = planned_now.scan.find("res/materials/imported.desc");
		BOOST_REQUIRE(file);
		BOOST_TEST((file->kind == pmg::file_class::outdated), file->reason);
		BOOST_TEST(!file->material_stages);
		BOOST_TEST(!planned(planned_now));
		BOOST_TEST(pmg::plan_refusal(planned_now).empty(), pmg::plan_refusal(planned_now));
	}
	BOOST_TEST(!rs->has_asset_index());

	pmg::command_context command{ copy.root(), *descs, *rs, &registry, {}, [] { return std::string{ "test-migration" }; }, {}, {}, {} };
	std::string printed;
	command.print = [&printed](std::string_view text) { printed += text; };
	const int code = pmg::run_migrate(pmg::migrate_command{}, command);
	BOOST_TEST(code == pmg::k_exit_done, printed);
	BOOST_TEST(!rs->has_asset_index());
	check_material_v2(read_json("res/materials/imported.desc"), "res://shaders/mix_opaque_trans_scene.shader.desc");
}

// A material over the engine's base with a fragment of its own (no engine pair): the type step refuses its stages, so
// it has material_stages. --migrate --dry-run checks it on a placeholder copy: it is planned, not invalid, and nothing
// is written.
BOOST_AUTO_TEST_CASE(AMaterialWithStagesOverTheEnginesBase_IsPlanned_InTheDryRunCommand)
{
	serve_engine_base();
	constexpr std::string_view glow = R"({
		"__type": "material_desc",
		"__parent": "res://base_material.desc",
		"shader_fragment": "res://shaders/glow.frag"
	})";
	write_text(at("res/materials/glow.desc"), glow);
	const auto before = content_of(copy.root());

	{
		const pmg::scoped_project_index index(*rs, copy.root(), read_only);
		const pmg::migration_plan planned_now = plan();
		const pmg::scanned_file* file = planned_now.scan.find("res/materials/glow.desc");
		BOOST_REQUIRE(file);
		BOOST_TEST((file->kind == pmg::file_class::outdated), file->reason);
		BOOST_TEST(file->material_stages);
	}

	pmg::command_context command{ copy.root(), *descs, *rs, &registry, {}, [] { return std::string{ "test-migration" }; }, {}, {}, {} };
	std::string printed;
	command.print = [&printed](std::string_view text) { printed += text; };
	pmg::migrate_command dry_run;
	dry_run.dry_run = true;
	const int code = pmg::run_migrate(dry_run, command);
	BOOST_TEST(code == pmg::k_exit_migration_needed, printed);
	BOOST_TEST(printed.find("Material shader stages to a .shader.desc") != std::string::npos, printed);
	BOOST_TEST(printed.find("- res/materials/glow.desc\n") != std::string::npos, printed);
	BOOST_TEST(printed.find("invalid") == std::string::npos, printed);
	BOOST_TEST(printed.find("Not migrated") == std::string::npos, printed);
	BOOST_TEST(read_text(at("res/materials/glow.desc")) == glow);
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(copy.root() / ".snk/migrations"));
}

// An inline material (a mesh node's in a prefab) is converted like a file: its file is named after the prefab.
BOOST_AUTO_TEST_CASE(AnInlineMaterialInAPrefab_GetsAShaderDescNamedAfterThePrefab)
{
	json::object material = json::parse(k_custom_pair).as_object();
	json::object mesh{ { "__type", "mesh_node_desc" }, { "__version", 1 }, { "material", material } };
	json::object node{ { "__id", "00000000000000c1" }, { "name", "mesh" }, { "components", json::object{ { "mesh_node_desc", mesh } } } };
	json::object prefab{ { "__type", "prefab_desc" }, { "__version", 4 }, { "children", json::array{ node } } };
	write_text(at("res/prefabs/rock.prefab.desc"), json::serialize(prefab));

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/prefabs/rock.prefab.shader.desc" }));
	BOOST_TEST(any_note(report, "res/prefabs/rock.prefab.shader.desc: created, referenced by res/prefabs/rock.prefab.desc at "
		"/children/0/components/mesh_node_desc/material"));
	// Wherever the engine's save of the prefab puts its components, the material is there once, at v2.
	const json::value migrated = json::parse(read_text(at("res/prefabs/rock.prefab.desc")));
	const json::object* found = find_material(migrated);
	BOOST_REQUIRE(found);
	check_material_v2(*found, "res://prefabs/rock.prefab.shader.desc");
}

// A stage written as {guid, path} keeps its guid in the new .shader.desc.
BOOST_AUTO_TEST_CASE(AGuidAndPathStage_IsKeptAsWritten)
{
	json::object material = json::parse(k_custom_pair).as_object();
	const json::object vertex{ { "guid", "6a0e2f1c-3b5d-4e7f-9a1b-2c3d4e5f6a7b" }, { "path", "res://shaders/custom.vert" } };
	material["shader_vertex"] = vertex;
	write_text(at("res/materials/rock.desc"), json::serialize(material));

	migrate();
	BOOST_TEST(read_json("res/materials/rock.shader.desc").at("vertex") == json::value(vertex));
}

// The same project gives the same names whatever order its files were written in (and the folder lists them in).
BOOST_AUTO_TEST_CASE(TheNames_DoNotDependOnTheOrderOfTheFiles)
{
	constexpr std::string_view other_pair = R"({"__type": "material_desc", "shader_vertex": "res://shaders/x.vert", "shader_fragment": "res://shaders/y.frag"})";
	const std::vector<std::pair<std::string, std::string_view>> files = {
		{ "res/materials/c.desc", k_custom_pair },
		{ "res/materials/a.desc", other_pair },
		{ "res/materials/b.desc", k_custom_pair },
		{ "res/materials/d.desc", other_pair },
	};
	for (const auto& [path, text] : files) write_text(copy.root() / path, text);
	for (auto it = files.rbegin(); it != files.rend(); ++it) write_text(other.root() / it->first, it->second);

	const pmg::migration_report first = migrate(copy.root());
	const pmg::migration_report second = migrate(other.root());
	const std::vector<std::string> expected{ "res/materials/a.shader.desc", "res/materials/b.shader.desc" };
	BOOST_TEST((created_shaders(first) == expected));
	BOOST_TEST((created_shaders(second) == expected));
	for (const auto& [path, text] : files) {
		BOOST_TEST(read_text(copy.root() / path) == read_text(other.root() / path), path);
	}
	BOOST_TEST(json::parse(read_text(copy.root() / "res/materials/c.desc")).at("shader").as_string() == "res://materials/b.shader.desc");
}

BOOST_AUTO_TEST_CASE(AStepFailingAfterIt_RemovesTheCreatedFiles)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	const auto before = content_of(copy.root());
	pmg::project_step failing;
	failing.id = "test.failing";
	failing.phase = pmg::project_phase::post;
	failing.summary = "a test step";
	failing.needed = [](const pmg::scan_result&) { return true; };
	failing.apply = [](pmg::project_step_context&) { throw std::runtime_error("injected step failure"); };
	registry.add(std::move(failing));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(at("res/materials/rock.shader.desc")));
	BOOST_TEST(!fs::exists(at("res/materials/rock.shader.desc.resource")));
}

// #1278: a file whose material the step rewrites is still dry-run, on a copy with a placeholder "shader" for it: the type
// steps of its other objects run, and the plan shows no diff for it (the copy's is not what is written).
BOOST_AUTO_TEST_CASE(AFileWithMaterialStages_IsDryRunOnAPlaceholderCopy_AndShowsNoDiff)
{
	int dry_runs = 0;
	register_test_type([&dry_runs](json::object& object, desc::migration_context&) {
		++dry_runs;
		object["hp"] = 2;
	});
	write_text(at("res/materials/mixed.desc"), k_with_other_type);
	const auto before = content_of(copy.root());

	pmg::plan_options options;
	options.project_steps = &registry;
	options.full_diff = true;
	const pmg::migration_plan planned_now = pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	BOOST_TEST(dry_runs > 0);
	BOOST_TEST(pmg::plan_refusal(planned_now).empty(), pmg::plan_refusal(planned_now));
	const pmg::scanned_file* file = planned_now.scan.find("res/materials/mixed.desc");
	BOOST_REQUIRE(file);
	BOOST_TEST((file->kind == pmg::file_class::outdated), file->reason);
	BOOST_TEST(file->material_stages);
	BOOST_TEST((file->material_stage_pointers == std::vector<std::string>{ "" }));

	const auto mixed = std::ranges::find(planned_now.files, std::string("res/materials/mixed.desc"), &pmg::file_plan::path);
	BOOST_REQUIRE((mixed != planned_now.files.end()));
	BOOST_TEST(mixed->diff.added == 0u);
	BOOST_TEST(mixed->diff.removed == 0u);
	BOOST_TEST(mixed->diff.changed == 0u);
	BOOST_TEST(!mixed->full_diff.has_value());
	BOOST_TEST(std::ranges::any_of(mixed->warnings, [](const desc::migration_warning& warning) {
		return warning.message.find("no diff in this plan") != std::string::npos;
	}));
	BOOST_TEST(std::ranges::any_of(mixed->steps, [](const pmg::step_use& step) { return step.type == "pmg_shaders_test_desc"; }));
	BOOST_TEST((content_of(copy.root()) == before));
}

// #1278: the other type of such a file fails on the copy: the file is invalid with that reason and no longer marked for
// the step, so --force leaves it whole (no .shader.desc for it) and the rest of the project migrates.
BOOST_AUTO_TEST_CASE(AFileWithMaterialStagesWhoseOtherTypeFails_IsInvalid_AndForceLeavesItWhole)
{
	register_test_type([](json::object&, desc::migration_context&) { throw std::runtime_error("injected type failure"); });
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	write_text(at("res/materials/mixed.desc"), k_with_other_type);

	const pmg::migration_plan strict = plan();
	const pmg::scanned_file* bad = strict.scan.find("res/materials/mixed.desc");
	BOOST_REQUIRE(bad);
	BOOST_TEST((bad->kind == pmg::file_class::invalid));
	BOOST_TEST(bad->reason.find("injected type failure") != std::string::npos, bad->reason);
	BOOST_TEST(!bad->material_stages);
	BOOST_TEST(bad->material_stage_pointers.empty());
	BOOST_TEST(!pmg::plan_refusal(strict).empty());
	BOOST_TEST(strict.scan.find("res/materials/rock.desc")->material_stages);

	const pmg::migration_plan forced = plan(copy.root(), true);
	BOOST_REQUIRE(pmg::plan_refusal(forced).empty());
	const pmg::apply_outcome outcome = pmg::apply_migration(forced, context(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial), outcome.report.error);
	BOOST_TEST((created_shaders(outcome.report) == std::vector<std::string>{ "res/materials/rock.shader.desc" }));
	BOOST_TEST(!fs::exists(at("res/materials/mixed.shader.desc")));
	BOOST_TEST(read_text(at("res/materials/mixed.desc")) == k_with_other_type);
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock.shader.desc");
}

// #1278: one checker serves the whole scan and answers as the one-shot check does, every time it is asked.
BOOST_AUTO_TEST_CASE(OneChecker_AnswersAsTheOneShotCheck)
{
	write_text(at("res/materials/rock.desc"), k_custom_pair);
	write_text(at("res/materials/orphan.desc"), R"({
		"__type": "material_desc",
		"__parent": "res://materials/missing.desc",
		"shader_fragment": "res://shaders/glow.frag"
	})");
	pmg::project_assets assets(copy.root(), { "res" }, {});
	assets.rebuild();
	const pmg::material_shaders_checker checker(copy.root(), assets);
	for (const std::string path : { "res/materials/rock.desc", "res/materials/orphan.desc", "res/materials/rock.desc" }) {
		const json::object material = read_json(path);
		BOOST_TEST(checker.problem(path, material) == pmg::material_shaders_problem(copy.root(), assets, path, material), path);
	}
	BOOST_TEST(checker.problem("res/materials/rock.desc", read_json("res/materials/rock.desc")).empty());
	const std::string orphan = checker.problem("res/materials/orphan.desc", read_json("res/materials/orphan.desc"));
	BOOST_TEST(orphan.find("res://materials/missing.desc") != std::string::npos, orphan);
}

BOOST_AUTO_TEST_SUITE_END()
