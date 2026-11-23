#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/desc/desc_schema.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/model_codec/res_tag.h>
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

	pmg::migration_plan plan(std::uintmax_t max_file_bytes = pmg::scan_options{}.max_file_bytes, pmg::geometry_check_cache* checks = nullptr)
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		options.scan.max_file_bytes = max_file_bytes;
		options.scan.geometry_checks = checks;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	pmg::migration_plan plan_with(pmg::geometry_check_cache& checks) { return plan(pmg::scan_options{}.max_file_bytes, &checks); }

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

	// The scan already runs the writer's checks: a desc they refuse is listed as not migrated with the reason, the step is
	// not planned for it, and nothing is written; after the migration the next scan says the same (the project does not
	// stay "needs a migration" forever).
	void is_left_by_the_scan(const std::string& desc_text, std::string_view reason_part)
	{
		write_text(at(k_desc), desc_text);
		for (int run = 0; run < 2; ++run) {
			const pmg::migration_plan planned_now = plan();
			BOOST_TEST(!planned(planned_now));
			const pmg::scanned_file* file = planned_now.scan.find(k_desc);
			BOOST_REQUIRE(file);
			BOOST_TEST(!file->geometry_arrays);
			BOOST_TEST(file->geometry_left.find(reason_part) != std::string::npos, file->geometry_left);
			const std::vector<pmg::skipped_file> skipped = pmg::skipped_by(planned_now);
			const auto listed = std::ranges::find(skipped, std::string(k_desc), &pmg::skipped_file::path);
			BOOST_REQUIRE(listed != skipped.end());
			BOOST_TEST(listed->reason.starts_with("its geometry stays in JSON arrays: "));
			BOOST_TEST(pmg::plan_text(planned_now).find("Not migrated:") != std::string::npos);
			if (run == 1) break;

			const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
			BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
			BOOST_TEST((outcome.report.status == pmg::migration_status::partial), outcome.report.error);
			BOOST_TEST(read_text(at(k_desc)) == desc_text);
			BOOST_TEST(!fs::exists(at(k_binary)));
		}
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

// Data the loader would refuse is left by the scan, with the check that refuses it.
BOOST_AUTO_TEST_CASE(AnIndexPastTheVertices_IsNotMigrated_AndNamesTheCheck)
{
	is_left_by_the_scan(arrays_desc({ 0, 1, 5 }), "/indices");
}

BOOST_AUTO_TEST_CASE(NoIndices_IsNotMigrated)
{
	is_left_by_the_scan(arrays_desc({}), "/indices: there are no indices");
}

BOOST_AUTO_TEST_CASE(NoLayoutOfItsOwn_IsNotMigrated)
{
	json::object desc = json::parse(arrays_desc({ 0, 1, 2 })).as_object();
	desc.erase("layout");
	desc["__parent"] = "res://geom/base.geom.desc";
	is_left_by_the_scan(json::serialize(desc), "geometry '");
}

// Stored bounds go (the loader builds them for a desc with ranges): only when they are the ones it builds.
BOOST_AUTO_TEST_CASE(StoredBoundsOtherThanTheLoaders_AreNotMigrated)
{
	const json::object bounds{ { "bounds", json::parse(R"([[[{"x":7,"y":7},{"x":9,"y":9}],0]])") } };
	is_left_by_the_scan(arrays_desc({ 0, 1, 2 }, bounds), "bounds");
}

BOOST_AUTO_TEST_CASE(StoredBoundsTheLoaderBuilds_AreMigrated)
{
	// One triangle without uv: the loader's box is the point (0, 0).
	const json::object bounds{ { "bounds", json::parse(R"([[[{"x":0,"y":0},{"x":0,"y":0}],0]])") } };
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }, bounds));
	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now));
	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	const json::object desc = json::parse(read_text(at(k_desc))).as_object();
	BOOST_TEST(desc.at("vertices").is_object());
	// Kept as the empty key the loader reads as "build them".
	BOOST_TEST(desc.contains("bounds"));
}

// A run stopped between creating the binary file and its journal leaves that very file beside the unchanged desc: the
// next run takes it as it is (it is not the run's to delete on a rollback, so it is not listed as created).
BOOST_AUTO_TEST_CASE(TheBinaryFileOfAnInterruptedRun_IsTakenAsItIs)
{
	const std::string arrays = arrays_desc({ 0, 1, 2 });
	write_text(at(k_desc), arrays);
	rnd::geometry_desc geometry;
	const json::object parsed = json::parse(arrays).as_object();
	BOOST_REQUIRE(rnd::decode_geometry_files(res::tag{ k_desc }, parsed, {}, geometry).empty());
	const rnd::geometry_files pair = rnd::encode_geometry_files(res::tag{ k_desc }, geometry);
	BOOST_REQUIRE(pair);
	const std::string binary(reinterpret_cast<const char*>(pair.binary.data()), pair.binary.size());
	write_text(at(k_binary), binary);

	const pmg::migration_plan planned_now = plan();
	BOOST_REQUIRE(planned(planned_now));
	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
	BOOST_REQUIRE_MESSAGE(outcome.report.status == pmg::migration_status::ok, outcome.report.error);
	BOOST_TEST(any_note(outcome.report, "which an interrupted run had already written"));
	BOOST_TEST((std::ranges::find(outcome.report.created_files, k_binary) == outcome.report.created_files.end()));
	BOOST_TEST(read_text(at(k_binary)) == binary);

	const json::object desc = json::parse(read_text(at(k_desc))).as_object();
	rnd::geometry_desc read;
	const std::string problem = rnd::decode_geometry_files(res::tag{ "res://geom/tri.geom.desc" }, desc, bytes_of(binary), read);
	BOOST_REQUIRE_MESSAGE(problem.empty(), problem);
	BOOST_TEST((read.vertices == vertex_bytes()));
}

// A geometry desc over the scan's limit is not read: it is listed as not migrated, and the migration is not refused for
// it as it is for any other file over the limit.
BOOST_AUTO_TEST_CASE(AGeometryDescTooBigForTheScan_IsNotMigrated_AndDoesNotStopTheMigration)
{
	constexpr std::uintmax_t limit = std::uintmax_t{ 1 } << 20;
	const std::string big = arrays_desc({ 0, 1, 2 }) + std::string(limit, ' ');
	write_text(at(k_desc), big);

	const pmg::migration_plan planned_now = plan(limit);
	BOOST_TEST(pmg::plan_refusal(planned_now).empty(), pmg::plan_refusal(planned_now));
	BOOST_TEST(!planned(planned_now));
	const pmg::scanned_file* file = planned_now.scan.find(k_desc);
	BOOST_REQUIRE(file);
	BOOST_TEST((file->kind != pmg::file_class::invalid));
	BOOST_TEST(file->geometry_left.find("too big for the scan") != std::string::npos, file->geometry_left);
	const std::vector<pmg::skipped_file> skipped = pmg::skipped_by(planned_now);
	BOOST_TEST((std::ranges::find(skipped, std::string(k_desc), &pmg::skipped_file::path) != skipped.end()));

	const pmg::apply_outcome outcome = pmg::apply_migration(planned_now, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial), outcome.report.error);
	BOOST_TEST(read_text(at(k_desc)) == big);
}

// #1155: the check encodes the whole pair, and a project not yet migrated is scanned on every open and every plan. A desc
// whose text has not changed takes the answer the cache kept; a changed one is checked again.
BOOST_AUTO_TEST_CASE(AnUnchangedDesc_TakesItsGeometryCheckFromTheCache_AChangedOneIsCheckedAgain)
{
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }));
	pmg::geometry_check_cache checks;
	BOOST_TEST(planned(plan_with(checks)));
	BOOST_TEST(checks.size() >= 1u);

	// An answer only the cache could give: the scan that reports it did not encode the desc again.
	pmg::geometry_check_cache primed;
	primed.problem(k_desc, read_text(at(k_desc)), [] { return std::string{ "remembered answer" }; });
	const pmg::migration_plan from_cache = plan_with(primed);
	const pmg::scanned_file* cached = from_cache.scan.find(k_desc);
	BOOST_REQUIRE(cached);
	BOOST_TEST(cached->geometry_left == "remembered answer");
	BOOST_TEST(!planned(from_cache));
	const std::size_t known = primed.size();

	write_text(at(k_desc), arrays_desc({ 0, 2, 1 }));
	const pmg::migration_plan changed = plan_with(primed);
	BOOST_REQUIRE(changed.scan.find(k_desc));
	BOOST_TEST(changed.scan.find(k_desc)->geometry_left.empty());
	BOOST_TEST(planned(changed));
	// The answer of the old text gave way to the new one: one answer a path.
	BOOST_TEST(primed.size() == known);
}

BOOST_AUTO_TEST_CASE(APlanWithTheCache_IsThePlanWithoutIt)
{
	write_text(at(k_desc), arrays_desc({ 0, 1, 2 }));
	write_text(at("res/geom/broken.geom.desc"), arrays_desc({ 0, 1, 5 }));
	pmg::geometry_check_cache checks;
	std::size_t after_first = 0;
	for (int run = 0; run < 2; ++run) {
		const pmg::migration_plan without = plan();
		const pmg::migration_plan with = plan_with(checks);
		BOOST_TEST(planned(with) == planned(without));
		for (const std::string_view path : { k_desc, std::string_view{ "res/geom/broken.geom.desc" } }) {
			const pmg::scanned_file* a = without.scan.find(path);
			const pmg::scanned_file* b = with.scan.find(path);
			BOOST_REQUIRE(a);
			BOOST_REQUIRE(b);
			BOOST_TEST(a->geometry_arrays == b->geometry_arrays);
			BOOST_TEST(a->geometry_left == b->geometry_left);
		}
		BOOST_TEST(pmg::skipped_by(with).size() == pmg::skipped_by(without).size());
		if (run == 0) after_first = checks.size();
	}
	BOOST_TEST(after_first >= 2u);
	BOOST_TEST(checks.size() == after_first);
}

BOOST_AUTO_TEST_SUITE_END()
