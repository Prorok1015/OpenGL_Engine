#include <boost/test/unit_test.hpp>
#include "edt_person_model_import.h"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_system.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// File > Import Model goes through the decoder process (#987): the steps are faked here, so this runs without assimp too.

namespace {

namespace fs = std::filesystem;

struct person_import_fixture
{
	test_util::temp_dir dir{ "edt_person_model_import" };
	fs::path model = dir.write("models/robot.glb", "glb-bytes");
	fs::path cache = dir.path() / "project" / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> resources;
	ds::task_context ctx;

	int scans = 0;
	int imports = 0;
	edt::import_source scanned;
	edt::import_source imported;
	std::vector<std::byte> scanned_bytes;
	std::vector<edt::referenced_file> imported_files;

	person_import_fixture()
	{
		dir.write("project/res/.keep", "");
		fs::create_directories(cache / "memory");
		fs::create_directories(cache / "user_profile");
		resources = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { dir.path() / "project" / "res" }, cache, cache / "user_profile"));
	}

	// A decoder that lists one file and imports a prefab, remembering what it was asked.
	edt::decoder_steps decoder()
	{
		edt::decoder_steps steps;
		steps.unavailable_reason = [] { return std::string{}; };
		steps.scan = [this](const edt::import_source& source, ds::task_context*) {
			++scans;
			scanned = source;
			if (source.preloaded) scanned_bytes = *source.preloaded;
			edt::model_file_scan found;
			edt::referenced_file side;
			side.path = dir.path() / "models" / "robot.bin";
			side.size = 4;
			found.files.push_back(side);
			found.total_bytes = 4;
			return found;
		};
		steps.import = [this](const edt::import_source& source, ds::task_context*) {
			++imports;
			imported = source;
			if (source.allowed) imported_files = *source.allowed;
			edt::import_intermediate result;
			result.prefab_tag = res::tag{ "memory://" + source.memory_folder + "robot.prefab.desc" };
			result.created_tags = { result.prefab_tag };
			return result;
		};
		return steps;
	}

	edt::import_intermediate run(const edt::decoder_steps& steps, const std::string& folder = "__imports/1/robot/")
	{
		return edt::person_model_import(model, folder, steps, *resources, ctx);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPersonModelImportTests, person_import_fixture)

BOOST_AUTO_TEST_CASE(TheModelGoesThroughTheDecoder_ScanThenImportWithTheListedFilesWithinTheTimeLimit)
{
	const auto before = std::chrono::steady_clock::now();
	const edt::import_intermediate result = run(decoder());

	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_TEST(scans == 1);
	BOOST_TEST(imports == 1);
	BOOST_TEST(std::string(reinterpret_cast<const char*>(scanned_bytes.data()), scanned_bytes.size()) == "glb-bytes");
	BOOST_TEST(scanned.path == model);
	BOOST_TEST(scanned.on_disk);
	BOOST_TEST(scanned.boundary.empty(), "the model's own folder is the boundary, as for an agent");
	BOOST_TEST(scanned.memory_folder == "__imports/1/robot/");
	BOOST_REQUIRE(imported_files.size() == 1u);
	BOOST_TEST(imported_files.front().path == dir.path() / "models" / "robot.bin");
	for (const edt::import_source* step : { &scanned, &imported }) {
		BOOST_TEST((step->deadline > before));
		BOOST_TEST((step->deadline <= std::chrono::steady_clock::now() + edt::k_person_parse_time_limit));
	}
	BOOST_TEST(result.prefab_tag.view() == "memory://__imports/1/robot/robot.prefab.desc");
	BOOST_TEST(result.source_path == model);
}

BOOST_AUTO_TEST_CASE(WithoutTheDecoder_TheImportFailsWithTheReason_AndReadsNothing)
{
	edt::decoder_steps steps = decoder();
	steps.unavailable_reason = [] { return std::string{ "'snk_model_decoder' is missing next to the editor" }; };
	const edt::import_intermediate result = run(steps);

	BOOST_TEST(result.error.find("'snk_model_decoder' is missing next to the editor") != std::string::npos, result.error);
	BOOST_TEST(scans == 0);
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(AFileTheModelMayNotRead_IsTheErrorOfTheListingStep_AndNothingIsImported)
{
	edt::decoder_steps steps = decoder();
	steps.scan = [](const edt::import_source&, ds::task_context*) {
		edt::model_file_scan found;
		found.error = "'../textures/wall.png' is outside the folder of the model";
		return found;
	};
	const edt::import_intermediate result = run(steps);

	BOOST_TEST(result.error.find("listing the files the model reads") != std::string::npos, result.error);
	BOOST_TEST(result.error.find("MiB") != std::string::npos, result.error);
	BOOST_TEST(result.error.find("'../textures/wall.png' is outside the folder of the model") != std::string::npos, result.error);
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(ADecoderThatFails_IsAnErrorOfItsStep_NotAnException)
{
	edt::decoder_steps steps = decoder();
	steps.import = [](const edt::import_source&, ds::task_context*) -> edt::import_intermediate {
		throw std::runtime_error("the decoder was killed: it ran past its time limit");
	};
	edt::import_intermediate result;
	BOOST_REQUIRE_NO_THROW(result = run(steps));
	BOOST_TEST(result.error.find("importing the model") != std::string::npos, result.error);
	BOOST_TEST(result.error.find("the decoder was killed") != std::string::npos, result.error);
}

BOOST_AUTO_TEST_CASE(AStopRequest_StartsNoDecoder)
{
	std::stop_source stop;
	stop.request_stop();
	ctx.stop = stop.get_token();
	BOOST_CHECK_THROW(run(decoder()), ds::task_cancelled);
	BOOST_TEST(scans == 0);
	BOOST_TEST(imports == 0);
}

BOOST_AUTO_TEST_CASE(AMissingFile_IsAnError)
{
	model = dir.path() / "models" / "gone.glb";
	const edt::import_intermediate result = run(decoder());
	BOOST_TEST(result.error.find("cannot be read") != std::string::npos, result.error);
	BOOST_TEST(scans == 0);
}

BOOST_AUTO_TEST_CASE(TheMemoryFolderIsEmptiedBeforeTheImport_SoAnEarlierRunLeavesNothingInTheWay)
{
	const res::tag left{ "memory://__imports/1/robot/robot.geom.desc" };
	BOOST_REQUIRE(resources->store_sync(left, std::vector<std::byte>(4, std::byte{ 1 })));
	bool present_at_import = true;
	edt::decoder_steps steps = decoder();
	steps.import = [&](const edt::import_source&, ds::task_context*) {
		present_at_import = resources->exists(left);
		return edt::import_intermediate{};
	};
	const edt::import_intermediate result = run(steps);
	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_TEST(!present_at_import);
}

BOOST_AUTO_TEST_CASE(AStopRequestWhileTheDecoderImports_LeavesNothingInMemory)
{
	// A cancelled job never reports back, so nobody else would erase what the decoder stored.
	std::stop_source stop;
	ctx.stop = stop.get_token();
	const res::tag stored{ "memory://__imports/1/robot/robot.prefab.desc" };
	edt::decoder_steps steps = decoder();
	steps.import = [&](const edt::import_source&, ds::task_context*) {
		BOOST_REQUIRE(resources->store_sync(stored, std::vector<std::byte>(4, std::byte{ 1 })));
		stop.request_stop();
		edt::import_intermediate result;
		result.prefab_tag = stored;
		result.created_tags = { stored };
		return result;
	};
	BOOST_CHECK_THROW(run(steps), ds::task_cancelled);
	BOOST_TEST(!resources->exists(stored));
}

BOOST_AUTO_TEST_CASE(TheFoldersOfEarlierImports_GoWithOneSweepOfImports)
{
	// erase_memory_folder takes every import below the folder it is given, and nothing beside it.
	const res::tag first{ "memory://__imports/1/robot/robot.geom.desc" };
	const res::tag second{ "memory://__imports/7/crate/materials/m.mat.desc" };
	const res::tag other{ "memory://__models/res/levels/robot.glb/robot.prefab.desc" };
	for (const res::tag& tag : { first, second, other }) {
		BOOST_REQUIRE(resources->store_sync(tag, std::vector<std::byte>(4, std::byte{ 1 })));
	}
	BOOST_TEST(!edt::erase_memory_folder(*resources, "__imports/").has_value());
	BOOST_TEST(!resources->exists(first));
	BOOST_TEST(!resources->exists(second));
	BOOST_TEST(resources->exists(other), "only the imports' folder is swept");
}

BOOST_AUTO_TEST_CASE(EachImportHasAFolderOfItsOwn_InTheFolderOfItsRun)
{
	BOOST_TEST(edt::person_import_folder(model, 42, 1) == "__imports/42/1/robot/");
	BOOST_TEST(edt::person_import_folder(model, 42, 2) == "__imports/42/2/robot/");
	BOOST_TEST(edt::person_import_folder(model, 43, 1) == "__imports/43/1/robot/");
	BOOST_TEST(edt::k_person_parse_time_limit.count() == 120);
	BOOST_TEST(edt::k_agent_parse_time_limit.count() == 60);
}

BOOST_AUTO_TEST_CASE(TheSweepAtStartup_ErasesOnlyTheImportsOfRunsThatEnded)
{
	// memory:// is shared by every editor on the project: another one's imports, and this run's, stay.
	constexpr std::int64_t ended_run = 2147483646;
	BOOST_REQUIRE(!platform::process_is_running(ended_run));
	const std::int64_t this_run = platform::current_process_id();
	const res::tag ended{ std::format("memory://__imports/{}/1/robot/robot.geom.desc", ended_run) };
	const res::tag ended_other{ std::format("memory://__imports/{}/7/crate/materials/m.mat.desc", ended_run) };
	const res::tag current{ std::format("memory://__imports/{}/1/robot/robot.geom.desc", this_run) };
	const res::tag not_a_run{ "memory://__imports/notes/robot.geom.desc" };
	// The layout before the run folders: "__imports/<serial>/<stem>/", erased even when a process has that number (pid 1).
	const res::tag earlier_layout{ "memory://__imports/1/robot/robot.geom.desc" };
	const res::tag other{ "memory://__models/res/levels/robot.glb/robot.prefab.desc" };
	for (const res::tag& tag : { ended, ended_other, current, not_a_run, earlier_layout, other }) {
		BOOST_REQUIRE(resources->store_sync(tag, std::vector<std::byte>(4, std::byte{ 1 })));
	}
	edt::erase_imports_of_ended_runs(*resources, this_run);
	BOOST_TEST(!resources->exists(ended));
	BOOST_TEST(!resources->exists(ended_other));
	BOOST_TEST(resources->exists(current), "this run's imports are not an earlier run's");
	BOOST_TEST(resources->exists(not_a_run), "a folder that names no process is left alone");
	BOOST_TEST(!resources->exists(earlier_layout));
	BOOST_TEST(resources->exists(other), "only the imports' folder is swept");
}

BOOST_AUTO_TEST_CASE(TheNameOfTheModelFile_ReachesTheLogOnlyEscaped)
{
	// U+202E (right-to-left override) and, where a file name may hold them, '\' and ':'.
#if defined(_WIN32)
	const std::string name = std::string{ "ro" } + "\xE2\x80\xAE" + "bot.glb";
	const std::string shown = "ro\\u{202E}bot.glb";
#else
	const std::string name = std::string{ "r\\o:" } + "\xE2\x80\xAE" + "bot.glb";
	const std::string shown = "r\\\\o:\\u{202E}bot.glb";
#endif
	model = dir.write(fs_paths::path_from_utf8("models/" + name), "glb-bytes");
	std::mutex mutex;
	std::vector<std::string> lines;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view category, engine::log_level, std::string_view message) {
		if (category != "Editor/Import") return;
		std::lock_guard lock(mutex);
		lines.emplace_back(message);
	});
	const edt::import_intermediate result = run(decoder());
	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	std::lock_guard lock(mutex);
	BOOST_REQUIRE(!lines.empty());
	for (const std::string& line : lines) {
		BOOST_TEST(line.find("\xE2\x80\xAE") == std::string::npos, line);
		BOOST_TEST(line.find(shown) != std::string::npos, line);
	}
}

BOOST_AUTO_TEST_SUITE_END()
