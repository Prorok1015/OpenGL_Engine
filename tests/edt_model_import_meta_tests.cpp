#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_exporter.h"
#include "edt_model_import_meta.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/command/cmd_types.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// The `.resource` of the prefab a model import writes (#1195): a new one with importer "model", the settings and a
// guid the asset database takes; a person's overwrite keeps its guid and flags and the settings a newer editor added,
// and refuses, before anything is written, a sidecar that is not canonical or of a newer version.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::string_view k_sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

edt::model_source_record rock()
{
	return { "rock.glb", std::string(k_sha) };
}

struct meta_fixture
{
	meta_fixture() { fs::create_directories(top()); }

	fs::path top() const { return dir.path() / "res"; }
	fs::path sidecar() const { return top() / "assets" / "rock" / "rock.prefab.desc.resource"; }
	res::tag prefab() const { return res::tag::make("assets/rock/rock.prefab.desc"); }

	// An existing sidecar as the editor writes it.
	res::asset_meta write_existing(const json::object& settings, std::vector<std::string> flags = { "keep" },
		std::string importer = "model") const
	{
		res::asset_meta meta;
		meta.guid = res::generate_asset_id();
		meta.importer = std::move(importer);
		meta.settings = settings;
		meta.flags = std::move(flags);
		dir.write("res/assets/rock/rock.prefab.desc.resource", res::write_asset_meta(meta));
		return meta;
	}

	res::asset_meta read_back() const
	{
		const res::asset_meta_result read = res::read_asset_meta(read_file(sidecar()));
		BOOST_REQUIRE_MESSAGE(read.meta.has_value(), read.error.message);
		return *read.meta;
	}

	test_util::temp_dir dir{ "model_import_meta" };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModelImportMeta, meta_fixture)

BOOST_AUTO_TEST_CASE(ANewPrefab_GetsANewSidecar_WithTheImporterTheSettingsAndTheSource)
{
	const edt::model_import_settings flat{ edt::model_normals::flat, false };
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), flat, rock(), false);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_CHECK(!plan.before.has_value());
	BOOST_CHECK(!fs::exists(sidecar()));
	BOOST_REQUIRE_MESSAGE(edt::write_model_meta(top(), plan).empty(), "written");

	const res::asset_meta meta = read_back();
	BOOST_CHECK(meta.guid == plan.guid);
	BOOST_CHECK_EQUAL(meta.importer, "model");
	BOOST_CHECK(meta.flags.empty());
	const edt::model_settings_parse settings = edt::parse_model_settings(meta.settings);
	BOOST_REQUIRE_MESSAGE(settings.ok(), settings.error);
	BOOST_CHECK(settings.settings == flat);
	BOOST_REQUIRE(settings.source.has_value());
	BOOST_CHECK(*settings.source == rock());
	// No path of the machine: the source is its file name.
	BOOST_CHECK(read_file(sidecar()).find(dir.path().filename().string()) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ANewSidecar_IsNeverWrittenOverAFile)
{
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(plan.ok());
	dir.write("res/assets/rock/rock.prefab.desc.resource", "made meanwhile");
	BOOST_CHECK(!edt::write_model_meta(top(), plan).empty());
	BOOST_CHECK_EQUAL(read_file(sidecar()), "made meanwhile");
}

BOOST_AUTO_TEST_CASE(AnAgent_NeverReplacesASidecar)
{
	write_existing(edt::model_settings_json({}, rock()));
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), false);
	BOOST_CHECK(!plan.ok());
	const bool named = plan.error.find("res://assets/rock/rock.prefab.desc.resource") != std::string::npos;
	BOOST_CHECK_MESSAGE(named, plan.error);
}

BOOST_AUTO_TEST_CASE(AnOverwrite_KeepsTheGuidAndFlags_AndReplacesTheSettings)
{
	const res::asset_meta before = write_existing(edt::model_settings_json({}, rock()), { "keep", "also" });
	const edt::model_import_settings flat{ edt::model_normals::flat, true };
	const edt::model_source_record other{ "rock_v2.glb", std::string(64, 'f') };
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), flat, other, true);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_REQUIRE(plan.before.has_value());
	BOOST_REQUIRE(edt::write_model_meta(top(), plan).empty());

	const res::asset_meta after = read_back();
	BOOST_CHECK(after.guid == before.guid);
	BOOST_CHECK(after.flags == before.flags);
	const edt::model_settings_parse settings = edt::parse_model_settings(after.settings);
	BOOST_REQUIRE_MESSAGE(settings.ok(), settings.error);
	BOOST_CHECK(settings.settings == flat);
	BOOST_CHECK(*settings.source == other);
	BOOST_CHECK_EQUAL(res::write_asset_meta(after), read_file(sidecar()));
}

BOOST_AUTO_TEST_CASE(AnOverwrite_KeepsTheSettingsANewerEditorOfVersionOneAdded)
{
	json::object settings = edt::model_settings_json({}, rock());
	settings["scale"] = 0.01;
	settings["up_axis"] = "z";
	const res::asset_meta before = write_existing(settings);
	const edt::model_meta_plan plan =
		edt::plan_model_meta(top(), prefab(), { edt::model_normals::flat, false }, rock(), true);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_REQUIRE(edt::write_model_meta(top(), plan).empty());

	const res::asset_meta after = read_back();
	BOOST_CHECK(after.guid == before.guid);
	BOOST_CHECK_EQUAL(after.settings.at("scale").as_double(), 0.01);
	BOOST_CHECK(after.settings.at("up_axis").as_string() == "z");
	BOOST_CHECK(after.settings.at("normals").as_string() == "flat");
	BOOST_CHECK(!after.settings.at("tangents").as_bool());
}

BOOST_AUTO_TEST_CASE(ASidecarOfANewerSettingsVersion_IsRefused_BeforeAnythingIsWritten)
{
	json::object settings = edt::model_settings_json({}, rock());
	settings["version"] = 2;
	write_existing(settings);
	const std::string bytes = read_file(sidecar());
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(!plan.ok());
	BOOST_CHECK_MESSAGE(plan.error.find("newer editor") != std::string::npos, plan.error);
	BOOST_CHECK(!edt::write_model_meta(top(), plan).empty());
	BOOST_CHECK_EQUAL(read_file(sidecar()), bytes);
}

BOOST_AUTO_TEST_CASE(ASidecarOfANewerFormat_IsRefused)
{
	const std::string newer = R"({"__type":"resource_meta","__version":99,"__guid":"x"})";
	dir.write("res/assets/rock/rock.prefab.desc.resource", newer);
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(!plan.ok());
	BOOST_CHECK_MESSAGE(plan.error.find("newer editor") != std::string::npos, plan.error);
	BOOST_CHECK_EQUAL(read_file(sidecar()), newer);
}

BOOST_AUTO_TEST_CASE(ASidecarNotInTheEditorsForm_IsRefused_AndNotRewritten)
{
	const res::asset_meta meta = write_existing(edt::model_settings_json({}, rock()));
	// The same content, written by hand on one line.
	const std::string by_hand = std::format(R"({{"__type":"resource_meta","__version":1,"__guid":"{}",)"
		R"("importer":"model","settings":{{}},"flags":[]}})", res::to_text(meta.guid));
	dir.write("res/assets/rock/rock.prefab.desc.resource", by_hand);
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(!plan.ok());
	BOOST_CHECK_MESSAGE(plan.error.find("form the editor writes") != std::string::npos, plan.error);
	BOOST_CHECK_EQUAL(read_file(sidecar()), by_hand);

	dir.write("res/assets/rock/rock.prefab.desc.resource", "not json");
	BOOST_CHECK(!edt::plan_model_meta(top(), prefab(), {}, rock(), true).ok());
	BOOST_CHECK_EQUAL(read_file(sidecar()), "not json");
}

BOOST_AUTO_TEST_CASE(ASidecarThatChangedAfterThePlan_IsNotRewritten)
{
	write_existing(edt::model_settings_json({}, rock()));
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(plan.ok());
	const res::asset_meta other = write_existing(json::object{}, {}, "");
	BOOST_CHECK(!edt::write_model_meta(top(), plan).empty());
	BOOST_CHECK(read_back().guid == other.guid);
}

BOOST_AUTO_TEST_CASE(TakingBack_RemovesANewSidecar_AndRestoresAReplacedOne)
{
	const edt::model_meta_plan fresh = edt::plan_model_meta(top(), prefab(), {}, rock(), true);
	BOOST_REQUIRE(edt::write_model_meta(top(), fresh).empty());
	BOOST_REQUIRE(edt::take_back_model_meta(top(), fresh).empty());
	BOOST_CHECK(!fs::exists(sidecar()));

	write_existing(edt::model_settings_json({}, rock()));
	const std::string bytes = read_file(sidecar());
	const edt::model_meta_plan replace =
		edt::plan_model_meta(top(), prefab(), { edt::model_normals::flat, false }, rock(), true);
	BOOST_REQUIRE(edt::write_model_meta(top(), replace).empty());
	BOOST_CHECK_NE(read_file(sidecar()), bytes);
	BOOST_REQUIRE(edt::take_back_model_meta(top(), replace).empty());
	BOOST_CHECK_EQUAL(read_file(sidecar()), bytes);
}

BOOST_AUTO_TEST_CASE(TheAssetDatabase_TakesTheGuidOfTheSidecar)
{
	const edt::model_meta_plan plan = edt::plan_model_meta(top(), prefab(), {}, rock(), false);
	BOOST_REQUIRE(edt::write_model_meta(top(), plan).empty());
	dir.write("res/assets/rock/rock.prefab.desc", R"({"__type":"prefab_desc"})");
	const std::string written = read_file(sidecar());

	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks };
	edt::asset_database walked(jobs, &events, std::vector<fs::path>{ top() });
	walked.rebuild();
	test_util::pump(tasks);
	const edt::asset_record* record = walked.find(prefab());
	BOOST_REQUIRE(record != nullptr);
	BOOST_REQUIRE(record->id.has_value());
	BOOST_CHECK(*record->id == plan.guid);
	BOOST_CHECK_EQUAL(read_file(sidecar()), written);
}

BOOST_AUTO_TEST_CASE(TheRecordOfAnImportedPrefab_IsRead_WithItsGuidFlagsSettingsAndSource)
{
	json::object settings = edt::model_settings_json({ edt::model_normals::flat, false }, rock());
	settings["a_newer_editors_key"] = 1;
	const res::asset_meta meta = write_existing(settings);
	const edt::model_import_record record = edt::read_model_import_record(top(), prefab());
	BOOST_REQUIRE_MESSAGE(record.ok(), record.error);
	BOOST_CHECK(record.guid == meta.guid);
	BOOST_CHECK(record.flags == std::vector<std::string>{ "keep" });
	BOOST_CHECK(record.settings.normals == edt::model_normals::flat);
	BOOST_CHECK(!record.settings.tangents);
	BOOST_REQUIRE(record.source.has_value());
	BOOST_CHECK(*record.source == rock());
	BOOST_CHECK_EQUAL(record.meta.view(), "res://assets/rock/rock.prefab.desc.resource");
}

BOOST_AUTO_TEST_CASE(ARecordMissingBrokenNewerOrOfAnotherImporter_IsRefused)
{
	BOOST_CHECK_EQUAL(edt::read_model_import_record(top(), prefab()).code, cmd::codes::not_found);

	write_existing(edt::model_settings_json({}, rock()), { "keep" }, "texture");
	BOOST_CHECK_EQUAL(edt::read_model_import_record(top(), prefab()).code, cmd::codes::invalid_args);

	dir.write("res/assets/rock/rock.prefab.desc.resource", "by hand");
	BOOST_CHECK_EQUAL(edt::read_model_import_record(top(), prefab()).code, cmd::codes::invalid_args);

	json::object newer_settings = edt::model_settings_json({}, rock());
	newer_settings["version"] = 2;
	write_existing(newer_settings);
	BOOST_CHECK_EQUAL(edt::read_model_import_record(top(), prefab()).code, cmd::codes::invalid_args);

	const res::asset_meta meta = write_existing(edt::model_settings_json({}, rock()));
	json::object newer = json::parse(res::write_asset_meta(meta)).as_object();
	newer["__version"] = 99;
	dir.write("res/assets/rock/rock.prefab.desc.resource", json::serialize(newer));
	const edt::model_import_record refused = edt::read_model_import_record(top(), prefab());
	BOOST_CHECK_EQUAL(refused.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(refused.error.find("newer editor") != std::string::npos, refused.error);
	BOOST_CHECK_MESSAGE(refused.error.find(dir.path().generic_string()) == std::string::npos, refused.error);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// The exporter over a res:// of its own, as the import writes into it.
struct export_fixture : test_util::scene_env
{
	export_fixture()
	{
		fs::create_directories(res_root);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
	}

	~export_fixture() { rs->unregistrate_resolver(res::tag::default_protocol()); }

	edt::import_result imported(std::string sha256 = std::string(k_sha))
	{
		edt::import_result result;
		result.source_path = fs::path("outside") / "rock.glb";
		result.root_tag = res::tag{ res::tag::memory, "rock/rock.prefab.desc" };
		const res::tag texture{ res::tag::memory, "rock/textures/albedo.png" };
		BOOST_REQUIRE(rs->store_sync(texture, test_util::to_bytes("png bytes")));
		BOOST_REQUIRE(rs->store_sync(result.root_tag, test_util::to_bytes(R"({"__type":"prefab_desc"})")));
		result.all_tags = { texture, result.root_tag };
		result.settings = { edt::model_normals::flat, true };
		result.source_sha256 = std::move(sha256);
		return result;
	}

	test_util::temp_dir dir{ "model_import_export_meta" };
	const fs::path res_root = dir.path() / "res";
	edt::asset_exporter exporter{ *rs };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModelImportMetaExport, export_fixture)

BOOST_AUTO_TEST_CASE(AnExport_WritesThePrefabsSidecar_AndNamesItAsWritten)
{
	const edt::import_result result = imported();
	const auto remap = exporter.build_remap(result, "rock", "assets/rock");
	std::vector<res::tag> written;
	edt::export_progress progress;
	BOOST_REQUIRE(exporter.export_to_project(result, remap, &progress, nullptr, &written, true, {}, res_root));

	const fs::path sidecar = res_root / "assets" / "rock" / "rock.prefab.desc.resource";
	const res::asset_meta_result read = res::read_asset_meta(read_file(sidecar));
	BOOST_REQUIRE(read.meta.has_value());
	BOOST_CHECK_EQUAL(read.meta->importer, "model");
	const edt::model_settings_parse settings = edt::parse_model_settings(read.meta->settings);
	BOOST_REQUIRE(settings.ok());
	BOOST_CHECK(settings.settings.normals == edt::model_normals::flat);
	BOOST_CHECK_EQUAL(settings.source->name, "rock.glb");
	bool named = false;
	for (const res::tag& tag : written) named = named || tag.view() == "res://assets/rock/rock.prefab.desc.resource";
	BOOST_CHECK(named);
	// The leaves have no sidecar of the import's.
	BOOST_CHECK(!fs::exists(res_root / "assets" / "rock" / "textures" / "albedo.png.resource"));
}

BOOST_AUTO_TEST_CASE(AnExportWithoutTheSourcesHash_WritesNoSidecar)
{
	const edt::import_result result = imported({});
	BOOST_REQUIRE(exporter.export_to_project(result, exporter.build_remap(result, "rock", "assets/rock"), nullptr,
		nullptr, nullptr, true, {}, res_root));
	BOOST_CHECK(fs::exists(res_root / "assets" / "rock" / "rock.prefab.desc"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "rock" / "rock.prefab.desc.resource"));
}

BOOST_AUTO_TEST_CASE(ASidecarThatIsRefused_StopsTheExportBeforeAnyFileIsWritten)
{
	dir.write("res/assets/rock/rock.prefab.desc.resource", "by hand");
	const edt::import_result result = imported();
	edt::export_progress progress;
	BOOST_CHECK(!exporter.export_to_project(result, exporter.build_remap(result, "rock", "assets/rock"), &progress,
		nullptr, nullptr, true, {}, res_root));
	BOOST_CHECK_MESSAGE(progress.error.find("rock.prefab.desc.resource") != std::string::npos, progress.error);
	BOOST_CHECK(!fs::exists(res_root / "assets" / "rock" / "textures"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "rock" / "rock.prefab.desc"));
	BOOST_CHECK_EQUAL(read_file(res_root / "assets" / "rock" / "rock.prefab.desc.resource"), "by hand");
}

BOOST_AUTO_TEST_CASE(AnOverwriteRefusedForAnAgent_WritesNothing)
{
	dir.write("res/assets/rock/rock.prefab.desc.resource", "anything");
	const edt::import_result result = imported();
	BOOST_CHECK(!exporter.export_to_project(result, exporter.build_remap(result, "rock", "assets/rock"), nullptr,
		nullptr, nullptr, true, {}, res_root, false));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "rock" / "textures"));
}

BOOST_AUTO_TEST_CASE(AReimport_KeepsThePrefabsGuidAndFlags_AndWritesItsSidecarAfterThePrefab)
{
	const edt::import_result first = imported();
	const auto remap = exporter.build_remap(first, "rock", "assets/rock");
	BOOST_REQUIRE(exporter.export_to_project(first, remap, nullptr, nullptr, nullptr, true, {}, res_root));
	const fs::path sidecar = res_root / "assets" / "rock" / "rock.prefab.desc.resource";
	res::asset_meta meta = *res::read_asset_meta(read_file(sidecar)).meta;
	meta.flags = { "keep" };
	dir.write("res/assets/rock/rock.prefab.desc.resource", res::write_asset_meta(meta));
	// The leaf's own sidecar, as the asset database made it.
	const std::string leaf_sidecar = res::write_asset_meta(res::asset_meta{ res::generate_asset_id(), {}, {}, {} });
	dir.write("res/assets/rock/textures/albedo.png.resource", leaf_sidecar);

	edt::import_result again = imported();
	again.settings = { edt::model_normals::smooth, false };
	BOOST_REQUIRE(rs->store_sync(again.root_tag, test_util::to_bytes(R"({"__type":"prefab_desc","name":"v2"})")));
	BOOST_REQUIRE(rs->store_sync(again.all_tags.front(), test_util::to_bytes("png bytes v2")));
	std::vector<res::tag> written;
	BOOST_REQUIRE(exporter.export_to_project(again, remap, nullptr, nullptr, &written, true, {}, res_root));

	const res::asset_meta after = *res::read_asset_meta(read_file(sidecar)).meta;
	BOOST_CHECK(after.guid == meta.guid);
	BOOST_CHECK(after.flags == std::vector<std::string>{ "keep" });
	const edt::model_settings_parse settings = edt::parse_model_settings(after.settings);
	BOOST_REQUIRE(settings.ok());
	BOOST_CHECK(settings.settings.normals == edt::model_normals::smooth);
	BOOST_CHECK(!settings.settings.tangents);
	// The leaf was overwritten in place; its sidecar, and so its ID, stays.
	BOOST_CHECK_EQUAL(read_file(res_root / "assets" / "rock" / "textures" / "albedo.png"), "png bytes v2");
	BOOST_CHECK_EQUAL(read_file(res_root / "assets" / "rock" / "textures" / "albedo.png.resource"), leaf_sidecar);
	// A sidecar that holds the guid already follows the prefab it describes.
	std::ptrdiff_t prefab_at = -1;
	std::ptrdiff_t meta_at = -1;
	for (std::size_t i = 0; i < written.size(); ++i) {
		if (written[i].view() == "res://assets/rock/rock.prefab.desc") prefab_at = static_cast<std::ptrdiff_t>(i);
		if (written[i].view() == "res://assets/rock/rock.prefab.desc.resource") meta_at = static_cast<std::ptrdiff_t>(i);
	}
	BOOST_REQUIRE(prefab_at >= 0 && meta_at >= 0);
	BOOST_CHECK_LT(prefab_at, meta_at);
}

BOOST_AUTO_TEST_CASE(FilesTheModelNoLongerMakes_AreListed_AndStayInTheProject)
{
	dir.write("res/assets/rock/textures/old.png", "old");
	const std::string old_prefab = R"({"__type":"prefab_desc","a":"res://assets/rock/textures/old.png",)"
		R"("b":{"guid":"x","path":"res://assets/rock/textures/albedo.png"},"c":"res://elsewhere/x.png"})";
	const edt::import_result result = imported();
	const auto remap = exporter.build_remap(result, "rock", "assets/rock");
	const std::vector<res::tag> left = edt::asset_exporter::no_longer_made(old_prefab, "assets/rock", remap);
	BOOST_REQUIRE_EQUAL(left.size(), 1u);
	BOOST_CHECK_EQUAL(left[0].view(), "res://assets/rock/textures/old.png");
	BOOST_REQUIRE(exporter.export_to_project(result, remap, nullptr, nullptr, nullptr, true, {}, res_root));
	BOOST_CHECK(fs::exists(res_root / "assets" / "rock" / "textures" / "old.png"));
	BOOST_CHECK(edt::asset_exporter::no_longer_made("not json", "assets/rock", remap).empty());
}

BOOST_AUTO_TEST_SUITE_END()
