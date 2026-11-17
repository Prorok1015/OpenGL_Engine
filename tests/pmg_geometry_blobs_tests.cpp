#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/desc/desc_schema.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// The project step engine.geometry_blobs (EPIC-79, #1129) on copies of the fixture of old versions (#750): a geometry desc
// with JSON arrays becomes a desc with ranges and its .bin, or stays as it is with a line of the report.
namespace {

constexpr std::string_view k_desc = "res/geom/tri.geom.desc";
constexpr std::string_view k_binary = "res/geom/tri.geom.bin";
constexpr std::string_view k_staged = "res/geom/tri.geom.bin.new";

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

// One triangle: three positions as the bytes of floats.
constexpr std::array<float, 9> k_positions = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f };

std::vector<std::byte> vertex_bytes()
{
	std::vector<std::byte> out(sizeof(k_positions));
	std::memcpy(out.data(), k_positions.data(), sizeof(k_positions));
	return out;
}

// A geometry desc in the form before EPIC-79; `extra` goes in after the type (a "topology", a "__guid").
std::string arrays_desc(const json::array& indices, const json::object& extra = {})
{
	json::object desc;
	desc["__type"] = "geometry_desc";
	desc["__version"] = 2;
	for (const auto& member : extra) desc[member.key()] = member.value();
	desc["layout"] = json::object{ { "position", "vec3" } };
	desc["indices"] = indices;
	json::array vertices;
	for (const std::byte b : vertex_bytes()) vertices.emplace_back(static_cast<std::uint64_t>(b));
	desc["vertices"] = std::move(vertices);
	return json::serialize(desc);
}

std::vector<std::byte> bytes_of(const std::string& text)
{
	std::vector<std::byte> out(text.size());
	if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
	return out;
}

bool any_note(const pmg::migration_report& report, std::string_view part)
{
	return std::ranges::any_of(report.project_notes, [part](const std::string& note) { return note.find(part) != std::string::npos; });
}

bool planned(const pmg::migration_plan& plan)
{
	return std::ranges::find(plan.project_steps, "engine.geometry_blobs", &pmg::planned_project_step::id) != plan.project_steps.end();
}

struct blobs_fixture : test_util::scene_env
{
	blobs_fixture()
	{
		// The fixture without its newer and broken files, which a migration refuses.
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		engine::render::register_render_descs(*descs);
		// The module of res/module/unknown.desc, loaded: the migration is complete.
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
		pmg::register_engine_project_steps(registry);
	}

	~blobs_fixture()
	{
		engine::render::unregister_render_descs(*descs);
	}

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	pmg::apply_context context()
	{
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry, {}, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	fs::path at(std::string_view path) const { return copy.root() / std::string(path); }

	void leaves_the_desc_beside(std::string_view beside)
	{
		write_text(at(k_desc), arrays_desc({ 0, 1, 2 }));
		write_text(at(beside), "not ours");
		const std::string desc = read_text(at(k_desc));

		const pmg::migration_plan planned_now = plan();
		BOOST_REQUIRE(planned(planned_now));
		const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
		BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
		BOOST_TEST(any_note(outcome.report, std::string(k_desc) + ": not migrated: " + std::string(beside) + " is already beside it"));
		BOOST_TEST(read_text(at(k_desc)) == desc);
		BOOST_TEST(read_text(at(beside)) == "not ours");
		BOOST_TEST(outcome.report.created_files.empty());
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry registry;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgGeometryBlobsTests, blobs_fixture)

BOOST_AUTO_TEST_CASE(ThePlanListsTheDesc_AndApplyWritesAPairThatReadsTheSameData)
{
	const json::object header{ { "__guid", "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53" } };
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }, header));

	const pmg::migration_plan before = plan();
	BOOST_REQUIRE(planned(before));
	const auto step = std::ranges::find(before.project_steps, "engine.geometry_blobs", &pmg::planned_project_step::id);
	BOOST_TEST(step->phase == "post");
	BOOST_TEST(before.scan.find(k_desc)->geometry_arrays);
	BOOST_TEST(pmg::plan_text(before).find(std::string(k_desc) + " -> " + std::string(k_binary)) != std::string::npos);

	const pmg::apply_outcome outcome = pmg::apply_migration(before, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_TEST(any_note(outcome.report, "vertices and indices moved to " + std::string(k_binary)));
	BOOST_TEST((std::ranges::find(outcome.report.created_files, k_binary) != outcome.report.created_files.end()));

	const std::string desc_text = read_text(at(k_desc));
	const json::object desc = json::parse(desc_text).as_object();
	BOOST_TEST(desc.at("vertices").is_object());
	BOOST_TEST(desc.at("indices").is_object());
	BOOST_TEST(desc.at("__version").as_int64() == rnd::geometry_binary_version);
	BOOST_TEST(desc.at("__guid").as_string() == "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53");

	// The data is copied, not computed: it reads back exactly.
	rnd::geometry_desc read;
	const std::vector<std::byte> binary = bytes_of(read_text(at(k_binary)));
	const std::string problem = rnd::decode_geometry_files(res::tag{ "res://geom/tri.geom.desc" }, desc, binary, read);
	BOOST_REQUIRE_MESSAGE(problem.empty(), problem);
	BOOST_TEST((read.vertices == vertex_bytes()));
	BOOST_TEST((read.indices == std::vector<unsigned int>{ 0, 1, 2 }));
	BOOST_TEST(read.layout.get_stride() == 3 * sizeof(float));

	// Done once: a second plan does not need the step.
	BOOST_TEST(!planned(plan()));
}

BOOST_AUTO_TEST_CASE(ARollback_PutsTheDescBack_AndDeletesTheBinaryFile)
{
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }));
	const auto before = content_of(copy.root());
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_REQUIRE(fs::exists(at(k_binary)));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(at(k_binary)));
}

BOOST_AUTO_TEST_CASE(AStepFailingAfterIt_RollsBackThePairToo)
{
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }));
	const auto before = content_of(copy.root());
	pmg::project_step failing;
	failing.id = "test.failing";
	failing.phase = pmg::project_phase::post;
	failing.summary = "a test step";
	failing.needed = [](const pmg::scan_result&) { return true; };
	failing.apply = [](pmg::project_step_context&) { throw std::runtime_error("injected step failure"); };
	registry.add(std::move(failing));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(at(k_binary)));
}

// A desc without an inner extension (base_geometry.desc, the sandbox's bench_quad.desc) can have no binary file, and lines
// keep their arrays (EPIC-79 PR3a): neither is planned, so a project of only such files has nothing to do.
BOOST_AUTO_TEST_CASE(NoInnerExtension_OrLines_IsNotPlanned)
{
	write_text(at("res/geom/quad.desc"), arrays_desc({ 0, 1, 2 }));
	write_text(at("res/geom/lines.geom.desc"), arrays_desc({ 0, 1, 1, 2 }, json::object{ { "topology", "lines" } }));

	const pmg::migration_plan planned_now = plan();
	BOOST_TEST(!planned(planned_now));
	BOOST_TEST(!planned_now.scan.find("res/geom/quad.desc")->geometry_arrays);
	BOOST_TEST(!planned_now.scan.find("res/geom/lines.geom.desc")->geometry_arrays);

	const std::string lines = read_text(at("res/geom/lines.geom.desc"));
	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_TEST(read_text(at("res/geom/lines.geom.desc")) == lines);
	BOOST_TEST(!fs::exists(at("res/geom/lines.geom.bin")));
}

// Something already at the name of the binary file or of its staged file: the desc is not migrated, nothing is written, and
// the rest of the migration goes on.
BOOST_AUTO_TEST_CASE(ABinaryFileAlreadyBeside_LeavesTheDescAsItIs)
{
	leaves_the_desc_beside(k_binary);
}

BOOST_AUTO_TEST_CASE(AStagedFileAlreadyBeside_LeavesTheDescAsItIs)
{
	leaves_the_desc_beside(k_staged);
}

// Data the loader would refuse (an index past the vertices) is reported with the check and written nowhere.
BOOST_AUTO_TEST_CASE(RefusedData_IsNotMigrated_AndNamesTheCheck)
{
	write_text(at(k_desc), arrays_desc({ 0, 1, 5 }));
	const std::string desc = read_text(at(k_desc));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_TEST(any_note(outcome.report, std::string(k_desc) + ": not migrated: "));
	BOOST_TEST(any_note(outcome.report, "/indices"));
	BOOST_TEST(read_text(at(k_desc)) == desc);
	BOOST_TEST(!fs::exists(at(k_binary)));
}

BOOST_AUTO_TEST_SUITE_END()
