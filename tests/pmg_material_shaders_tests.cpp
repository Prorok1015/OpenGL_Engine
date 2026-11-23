#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
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

	pmg::migration_plan plan(const fs::path& root)
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		return pmg::plan_migration(root, pmg::scan_project(root, *descs, options.scan), *descs, options);
	}

	pmg::migration_plan plan() { return plan(copy.root()); }

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

	test_util::fixture_copy copy{ "migration_v1" };
	// A second project for the runs that are compared with the first.
	test_util::fixture_copy other{ "migration_v1" };
	pmg::project_step_registry registry;
};

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
	write_text(at("res/materials/rock.shader.desc"), "not ours");

	const pmg::migration_report report = migrate();
	BOOST_TEST((created_shaders(report) == std::vector<std::string>{ "res/materials/rock-2.shader.desc" }));
	BOOST_TEST(read_text(at("res/materials/rock.shader.desc")) == "not ours");
	check_material_v2(read_json("res/materials/rock.desc"), "res://materials/rock-2.shader.desc");
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

BOOST_AUTO_TEST_SUITE_END()
