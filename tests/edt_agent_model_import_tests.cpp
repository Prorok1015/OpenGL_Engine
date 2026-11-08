#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_model_import.h"
#include "edt_file_menu.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json/parse.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// The model import an agent asks for (#548): the file a person is shown is the file that is imported, nothing is
// written when the import fails or is cancelled, and nothing of the import stays in memory://. The model itself is
// faked (a few memory:// resources), so these tests need no assimp.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct import_fixture : test_util::scene_env
{
	import_fixture()
	{
		fs::create_directories(res_root);
		use_policy(res::write_policy::allow_all());
		dir.write("outside/robot.glb", "glb bytes of the robot");
		source = fs::weakly_canonical(dir.path() / "outside" / "robot.glb");
		service = std::make_unique<edt::agent_model_import>(exporter, jobs, *rs, options());
	}

	~import_fixture()
	{
		service.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	void use_policy(res::write_policy policy)
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
			res::mount_access::read_write, std::move(policy));
	}

	edt::agent_import_options options()
	{
		edt::agent_import_options result;
		result.import = [this](const edt::import_source& source, ds::task_context* task) { return fake_import(source, task); };
		result.scan = [this](const edt::import_source& source, ds::task_context*) {
			++scans;
			deadline_was_set = source.deadline != std::chrono::steady_clock::time_point{};
			edt::model_file_scan scan;
			scan.error = scan_error;
			scan.files = listed_files;
			for (const auto& file : listed_files) scan.total_bytes += file.size;
			return scan;
		};
		result.resource_layers = [this] { return std::vector<fs::path>{ res_root }; };
		result.resources_path = [this] { return res_root; };
		result.blocker = [this]() -> std::optional<std::string> { return blocked; };
		result.now = [this] { return clock; };
		result.max_source_bytes = 4096;
		return result;
	}

	// What assimp would leave in memory://: textures, a geometry and a prefab that refers to it.
	edt::import_intermediate fake_import(const edt::import_source& source, ds::task_context*)
	{
		const fs::path& model = source.path;
		++imports;
		if (source.allowed) seen_allowed = *source.allowed;
		if (source.preloaded) seen_bytes.assign(reinterpret_cast<const char*>(source.preloaded->data()), source.preloaded->size());
		if (during_import) during_import();
		edt::import_intermediate imported;
		if (fail_import) {
			imported.error = "the model is broken";
			return imported;
		}
		const std::string stem = model.stem().string();
		const res::tag texture{ "memory://" + stem + "/tex/albedo.png" };
		const res::tag geometry{ "memory://" + stem + "/" + stem + ".geom.desc" };
		const res::tag prefab{ "memory://" + stem + "/" + stem + ".prefab.desc" };
		rs->store(texture, test_util::to_bytes("png bytes"));
		rs->store(geometry, test_util::to_bytes(R"({"__type":"geometry_desc"})"));
		rs->store(prefab, test_util::to_bytes(R"({"geometry":")" + std::string(geometry.view()) + R"("})"));
		imported.all_tags = { texture, geometry, prefab };
		imported.created_tags = imported.all_tags;
		if (foreign_tag) imported.all_tags.push_back(*foreign_tag);
		imported.geom_tag = geometry;
		imported.prefab_tag = prefab;
		imported.source_path = model;
		return imported;
	}

	bool in_memory(const char* tag)
	{
		auto raw = rs->fetch_data(res::tag{ tag });
		if (!raw) return false;
		raw->wait();
		return raw->is_ready();
	}

	void run_jobs()
	{
		while (tasks.run_one(ds::lane::compute)) {}
		tasks.drain_main();
	}

	edt::agent_import_request request(const std::string& name = {}, const std::string& folder = {}) const
	{
		return { source.string(), name, folder };
	}

	json::object finished(std::uint64_t job)
	{
		run_jobs();
		const auto info = jobs.info(job);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
		BOOST_REQUIRE(info->result.has_value());
		return info->result->as_object();
	}

	// The question, the person's yes, the start: as the registry does it for an agent.
	edt::agent_model_import::started approved_start(const edt::agent_import_request& wanted, const cmd::origin& caller)
	{
		const auto shown = service->describe(wanted, caller);
		BOOST_REQUIRE(shown.has_value());
		return service->start(wanted, caller, true);
	}

	test_util::temp_dir dir{ "agent_model_import" };
	const fs::path res_root = dir.path() / "res";
	fs::path source;
	edt::job_service jobs{ tasks };
	edt::asset_exporter exporter{ *rs };
	std::unique_ptr<edt::agent_model_import> service;
	const cmd::origin agent = cmd::origin::agent("session-1");
	std::chrono::steady_clock::time_point clock{};
	std::optional<std::string> blocked;
	bool fail_import = false;
	int imports = 0;
	int scans = 0;
	bool deadline_was_set = false;
	std::string scan_error;
	std::vector<edt::referenced_file> listed_files;
	std::vector<edt::referenced_file> seen_allowed;
	std::string seen_bytes;
	std::optional<res::tag> foreign_tag;
	std::function<void()> during_import;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentModelImportTests, import_fixture)

BOOST_AUTO_TEST_CASE(Resolve_NamesTheFileTheNameAndTheFilesItWillWrite)
{
	const edt::agent_import_plan plan = service->resolve(request());
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_CHECK(plan.source == source);
	BOOST_CHECK_EQUAL(plan.size, 22u);
	BOOST_CHECK_EQUAL(plan.name, "robot");
	BOOST_CHECK_EQUAL(plan.folder, "assets/robot");
	BOOST_CHECK_EQUAL(plan.prefab, "res://assets/robot/robot.prefab.desc");

	const edt::agent_import_plan named = service->resolve(request("hero", "res://models/heroes"));
	BOOST_REQUIRE_MESSAGE(named.ok(), named.error);
	BOOST_CHECK_EQUAL(named.prefab, "res://models/heroes/hero.prefab.desc");
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesWhatIsNoModelFileOnDisk)
{
	const auto code_of = [this](const edt::agent_import_request& wanted) {
		const edt::agent_import_plan plan = service->resolve(wanted);
		BOOST_CHECK(!plan.ok());
		return plan.code;
	};
	dir.write("outside/notes.txt", "text");
	dir.write("outside/empty.glb", "");
	dir.write("outside/big.glb", std::string(5000, 'x'));
	fs::create_directories(dir.path() / "outside" / "folder.glb");

	BOOST_CHECK_EQUAL(code_of({ "robot.glb", "", "" }), cmd::codes::invalid_args);	// not absolute
	BOOST_CHECK_EQUAL(code_of({ (dir.path() / "outside" / "missing.glb").string(), "", "" }), cmd::codes::not_found);
	BOOST_CHECK_EQUAL(code_of({ (dir.path() / "outside" / "notes.txt").string(), "", "" }), cmd::codes::invalid_args);	// no model extension
	BOOST_CHECK_EQUAL(code_of({ (dir.path() / "outside" / "empty.glb").string(), "", "" }), cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(code_of({ (dir.path() / "outside" / "folder.glb").string(), "", "" }), cmd::codes::invalid_args);	// a folder
	const edt::agent_import_plan big = service->resolve({ (dir.path() / "outside" / "big.glb").string(), "", "" });
	BOOST_CHECK(!big.ok());
	BOOST_CHECK_EQUAL(big.code, cmd::codes::invalid_args);
	BOOST_CHECK(big.error.find("too big") != std::string::npos);
	BOOST_CHECK(big.error.find("5000") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesALinkToAModel)
{
	std::error_code ec;
	const fs::path link = dir.path() / "outside" / "link.glb";
	fs::create_symlink(source, link, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const edt::agent_import_plan plan = service->resolve({ link.string(), "", "" });
	BOOST_CHECK(!plan.ok());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK(plan.error.find("link") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesBadNamesAndFolders)
{
	for (const char* name : { "..", "a/b", "CON", "x.", "a:b", "a|b" }) {
		BOOST_TEST_CONTEXT("name " << name) {
			const edt::agent_import_plan plan = service->resolve(request(name));
			BOOST_CHECK(!plan.ok());
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
		}
	}
	for (const char* folder : { "../out", "/abs", "C:/x", "memory://x", "a//b", "a/./b", "a\\b", "nul/x", "a/b." }) {
		BOOST_TEST_CONTEXT("folder " << folder) {
			const edt::agent_import_plan plan = service->resolve(request("", folder));
			BOOST_CHECK(!plan.ok());
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
		}
	}
}

BOOST_AUTO_TEST_CASE(Resolve_AnExistingTargetNeedsInput_InAnyLayer)
{
	dir.write("res/assets/robot/robot.prefab.desc", "{}");
	const edt::agent_import_plan plan = service->resolve(request());
	BOOST_CHECK(!plan.ok());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::needs_input);
	BOOST_CHECK(plan.error.find("res://assets/robot/robot.prefab.desc") != std::string::npos);
	BOOST_CHECK(service->resolve(request("other")).ok());
}

BOOST_AUTO_TEST_CASE(Resolve_IsBusyWhileAnotherImportRunsOrTheEditorBlocks)
{
	blocked = "Stop Play to import.";
	const edt::agent_import_plan plan = service->resolve(request());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::busy);
	BOOST_CHECK_EQUAL(plan.error, "Stop Play to import.");
	blocked.reset();

	BOOST_REQUIRE(service->start(request(), cmd::origin::ui(), false).ok());
	BOOST_CHECK(service->running());
	BOOST_CHECK_EQUAL(service->resolve(request("second")).code, cmd::codes::busy);
	run_jobs();
	BOOST_CHECK(!service->running());
	BOOST_CHECK(service->resolve(request("second")).ok());
}

BOOST_AUTO_TEST_CASE(WithoutAnImporter_EverythingIsDisabledWithTheReason)
{
	edt::agent_import_options none = options();
	none.import = nullptr;
	edt::agent_model_import bare(exporter, jobs, *rs, std::move(none));
	BOOST_CHECK(!bare.available());
	const edt::agent_import_plan plan = bare.resolve(request());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::disabled);
	BOOST_CHECK(plan.error.find("assimp") != std::string::npos);
	const auto started = bare.start(request(), cmd::origin::ui(), true);
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::disabled);
}

BOOST_AUTO_TEST_CASE(Describe_ShowsTheFullPathTheSizeAndTheDestination)
{
	const std::optional<std::string> text = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(source.string()) != std::string::npos);
	BOOST_CHECK(text->find("22 bytes") != std::string::npos);
	BOOST_CHECK(text->find("res://models/") != std::string::npos);
	BOOST_CHECK(text->find("'hero'") != std::string::npos);
	BOOST_CHECK(text->find("res://models/hero.prefab.desc") != std::string::npos);

	const std::optional<std::string> refused = service->describe({ "relative.glb", "", "" }, agent);
	BOOST_REQUIRE(refused.has_value());
	BOOST_CHECK(refused->rfind("This request will be refused", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(Describe_ShowsOnlyThePathTheSizeAndTheDestination_AndParsesNothing)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const std::optional<std::string> text = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(source.string()) != std::string::npos);
	BOOST_CHECK(text->find("skin.png") == std::string::npos);
	BOOST_CHECK(text->find("not read until") != std::string::npos);
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(ASelfContainedModel_NeedsOnePrompt)
{
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	const json::object result = finished(started.job);
	BOOST_CHECK(!result.contains("needs_files_approval"));
	BOOST_CHECK_EQUAL(std::string(result.at("prefab").as_string()), "res://models/hero.prefab.desc");
	BOOST_CHECK_EQUAL(scans, 1);
	BOOST_CHECK_EQUAL(imports, 1);
	BOOST_CHECK(deadline_was_set);
}

BOOST_AUTO_TEST_CASE(AModelWithFiles_EndsTheFirstJobWithTheirListAndWritesNothing)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 }, { dir.path() / "outside" / "mesh.bin", 3000 } };
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	const json::object result = finished(started.job);
	BOOST_CHECK(result.at("needs_files_approval").as_bool());
	BOOST_CHECK(!std::string(result.at("files_token").as_string()).empty());
	BOOST_CHECK_EQUAL(result.at("files").as_array().size(), 2u);
	BOOST_CHECK_EQUAL(result.at("total_bytes").as_int64(), 4000);
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
	BOOST_CHECK(!in_memory("memory://robot/robot.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(TheSecondCall_ShowsEveryFileWithTheTotal_AndImportsExactlyThose)
{
	for (int i = 0; i < 25; ++i) listed_files.push_back({ dir.path() / "outside" / ("tex" + std::to_string(i) + ".png"), 100 });
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	const std::string token(first.at("files_token").as_string());

	edt::agent_import_request second = request("hero", "models");
	second.files_token = token;
	const std::optional<std::string> text = service->describe(second, agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->rfind("This request will be refused", 0) != 0u);
	for (const auto& file : listed_files) BOOST_TEST_CONTEXT(file.path.string()) { BOOST_CHECK(text->find(file.path.string()) != std::string::npos); }
	BOOST_CHECK(text->find("25 files") != std::string::npos);
	BOOST_CHECK(text->find("2500 bytes") != std::string::npos);

	const auto started = service->start(second, agent, true);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	scan_error = "must not be asked again";
	const json::object result = finished(started.job);
	BOOST_CHECK_EQUAL(std::string(result.at("prefab").as_string()), "res://models/hero.prefab.desc");
	BOOST_CHECK_EQUAL(scans, 1);
	BOOST_CHECK_EQUAL(imports, 1);
	BOOST_REQUIRE_EQUAL(seen_allowed.size(), 25u);
	BOOST_CHECK_EQUAL(seen_bytes, "glb bytes of the robot");
}

BOOST_AUTO_TEST_CASE(TheSecondCall_WithoutAListOfItsOwn_IsRefused)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	const std::string token(first.at("files_token").as_string());

	edt::agent_import_request wrong_token = request("hero", "models");
	wrong_token.files_token = "files-999";
	BOOST_CHECK(service->describe(wrong_token, agent)->rfind("This request will be refused", 0) == 0u);
	BOOST_CHECK_EQUAL(service->start(wrong_token, agent, true).code, cmd::codes::denied);

	edt::agent_import_request other_name = request("villain", "models");
	other_name.files_token = token;
	BOOST_CHECK(service->describe(other_name, agent)->rfind("This request will be refused", 0) == 0u);

	edt::agent_import_request other_session = request("hero", "models");
	other_session.files_token = token;
	BOOST_CHECK(service->describe(other_session, cmd::origin::agent("session-2"))->rfind("This request will be refused", 0) == 0u);

	clock += std::chrono::minutes(6);
	edt::agent_import_request late = request("hero", "models");
	late.files_token = token;
	BOOST_CHECK(service->describe(late, agent)->rfind("This request will be refused", 0) == 0u);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(ADeniedSecondQuestion_LeavesNothingInTheProject)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = "whatever";
	BOOST_CHECK_EQUAL(service->start(second, agent, false).code, cmd::codes::denied);
	run_jobs();
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

BOOST_AUTO_TEST_CASE(AModelTheScanRefuses_FailsTheJobWithTheReason_AndWritesNothing)
{
	scan_error = "the model refers to '.ssh/id_rsa': a hidden file or folder";
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(info->error.find("hidden") != std::string::npos);
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

BOOST_AUTO_TEST_CASE(Start_WithoutAnApproval_LooksAtNoPath)
{
	const auto missing = service->start({ (dir.path() / "no" / "such.glb").string(), "", "" }, agent, false);
	BOOST_CHECK_EQUAL(missing.code, cmd::codes::denied);
	const auto present = service->start(request(), agent, false);
	BOOST_CHECK_EQUAL(present.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(missing.error, present.error);
	BOOST_CHECK_EQUAL(scans, 0);
}

BOOST_AUTO_TEST_CASE(Job_ErasesOnlyTheMemoryCopiesItCreated)
{
	const res::tag foreign{ "memory://robot/tex/foreign.png" };
	rs->store(foreign, test_util::to_bytes("belongs to somebody else"));
	foreign_tag = foreign;
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();
	BOOST_CHECK(in_memory("memory://robot/tex/foreign.png"));
	BOOST_CHECK(!in_memory("memory://robot/robot.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(Start_AfterTheApproval_ImportsExportsAndCleansMemory)
{
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	BOOST_CHECK_GT(started.job, 0u);

	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
	BOOST_REQUIRE(info->result.has_value());
	const json::object& result = info->result->as_object();
	BOOST_CHECK_EQUAL(std::string(result.at("prefab").as_string()), "res://models/hero.prefab.desc");
	std::vector<std::string> assets;
	for (const json::value& asset : result.at("assets").as_array()) assets.emplace_back(asset.as_string());
	BOOST_TEST((assets == std::vector<std::string>{ "res://models/hero.geom.desc", "res://models/hero.prefab.desc", "res://models/tex/albedo.png" }));

	BOOST_CHECK(fs::exists(res_root / "models" / "hero.prefab.desc"));
	BOOST_CHECK(fs::exists(res_root / "models" / "hero.geom.desc"));
	BOOST_CHECK_EQUAL(read_file(res_root / "models" / "tex" / "albedo.png"), "png bytes");
	// The memory:// references of the prefab point at the project files now.
	BOOST_CHECK(read_file(res_root / "models" / "hero.prefab.desc").find("res://models/hero.geom.desc") != std::string::npos);
	for (const char* tag : { "memory://robot/tex/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
	BOOST_CHECK(!service->running());
}

BOOST_AUTO_TEST_CASE(Start_IsNotFreeForAnAgent_ItNeedsTheShownApproval)
{
	// Never shown: nothing starts.
	auto started = service->start(request(), agent, true);
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// Shown but not approved (the call carries no ticket).
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	started = service->start(request(), agent, false);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// Shown to another session.
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	started = service->start(request(), cmd::origin::agent("session-2"), true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// The approval is used up by the start.
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	BOOST_REQUIRE(service->start(request(), agent, true).ok());
	run_jobs();
	started = service->start(request("again"), agent, true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 1);
}

BOOST_AUTO_TEST_CASE(Start_RefusesAnApprovalThatIsTooOld)
{
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	clock += edt::agent_model_import::k_shown_lifetime + std::chrono::seconds(1);
	const auto started = service->start(request(), agent, true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(Start_RefusesAFileThatChangedSinceItWasShown)
{
	// Other bytes (the size).
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	dir.write("outside/robot.glb", "glb bytes of a much longer and different robot");
	auto started = service->start(request(), agent, true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// The same size, another time.
	dir.write("outside/robot.glb", "glb bytes of the robot");
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	fs::last_write_time(source, fs::last_write_time(source) + std::chrono::hours(1));
	started = service->start(request(), agent, true);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(Job_RefusesAFileThatChangesWhileItIsImported_AndLeavesNothing)
{
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	during_import = [this] { dir.write("outside/robot.glb", "swapped while it was read, and longer"); };
	run_jobs();

	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(info->error.find("changed") != std::string::npos);
	BOOST_CHECK(!fs::exists(res_root / "assets"));
	for (const char* tag : { "memory://robot/tex/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
}

BOOST_AUTO_TEST_CASE(Job_ABrokenModelFailsAndLeavesNothing)
{
	fail_import = true;
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_EQUAL(info->error, "the model is broken");
	BOOST_CHECK(!fs::exists(res_root / "assets"));
}

BOOST_AUTO_TEST_CASE(Job_ATargetThatAppearedMeanwhile_WritesNothing)
{
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	// Not the prefab (that one is checked when the question is put), a file the import will also write.
	dir.write("res/assets/robot/robot.geom.desc", "somebody's file");
	run_jobs();

	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(info->error.find("robot.geom.desc") != std::string::npos);
	BOOST_CHECK(info->error.find("exists") != std::string::npos);
	BOOST_CHECK_EQUAL(read_file(res_root / "assets" / "robot" / "robot.geom.desc"), "somebody's file");
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "tex"));
	BOOST_CHECK(!in_memory("memory://robot/robot.prefab.desc"));
}

// The write of one file is refused half way: the files written before it are taken back, the prefab is never written.
BOOST_AUTO_TEST_CASE(Job_AWriteRefusedHalfWay_TakesBackWhatWasWritten)
{
	use_policy(res::write_policy(res::tag::default_protocol(), { "res://**", "!res://assets/robot/robot.geom.desc" }));
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();

	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "tex" / "albedo.png"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.geom.desc"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
	for (const char* tag : { "memory://robot/tex/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
}

BOOST_AUTO_TEST_CASE(Job_CancelledBeforeTheWriteLeavesNoFileAndNoMemoryCopy)
{
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	during_import = [this, job = started.job] { jobs.cancel(job); };
	run_jobs();

	BOOST_CHECK(!jobs.is_running(started.job));
	BOOST_CHECK_EQUAL(imports, 1);
	BOOST_CHECK(!fs::exists(res_root / "assets"));
	for (const char* tag : { "memory://robot/tex/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
	BOOST_CHECK(!service->running());
}

BOOST_AUTO_TEST_CASE(Job_CancelledBeforeItRunsNeverImports)
{
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	BOOST_REQUIRE(jobs.cancel(started.job));
	run_jobs();
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "assets"));
}

BOOST_AUTO_TEST_CASE(Progress_StartsAtTheModelAndIsReportedToTheJob)
{
	std::string note;
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	during_import = [this, &note, job = started.job] {
		if (const auto progress = jobs.progress(job)) note = progress->note;
	};
	run_jobs();
	BOOST_CHECK_EQUAL(note, "Reading the model");
}

BOOST_AUTO_TEST_CASE(APersonsOwnCall_NeedsNoApproval)
{
	const auto started = service->start(request("mine", "models"), cmd::origin::ui(), false);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs();
	BOOST_CHECK_EQUAL(scans, 1);
	BOOST_CHECK(fs::exists(res_root / "models" / "mine.prefab.desc"));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// The File menu action over the service: what the registry and the policy see.
struct import_action_fixture : import_fixture
{
	import_action_fixture()
	{
		hooks.import_model = [this](const fs::path& model) { dialog_imports.push_back(model.string()); };
		hooks.agent_import = service.get();
		menu = std::make_unique<edt::file_menu>(dialogs, actions, hooks);
		menu->register_actions();
	}

	~import_action_fixture() { menu.reset(); }

	cmd::result call(const cmd::origin& from, std::string_view args, std::string confirmation = {})
	{
		cmd::call request;
		request.args = json::parse(args);
		request.from = from;
		request.confirmation = std::move(confirmation);
		return actions.invoke("file.import_model", request);
	}

	std::string path_json() const { return json::serialize(json::object{ { "path", source.string() } }); }

	edt::dialog_service dialogs;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
	std::vector<std::string> dialog_imports;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtImportModelActionTests, import_action_fixture)

BOOST_AUTO_TEST_CASE(TheCommandAsksAPersonAndStartsAJob)
{
	const cmd::command_desc* command = actions.commands().find("file.import_model");
	BOOST_REQUIRE(command != nullptr);
	BOOST_CHECK((command->category == cmd::agent_category::scene_edit));
	BOOST_CHECK((command->risk == cmd::danger::confirm));
	BOOST_CHECK(command->starts_job);
	BOOST_CHECK(command->mutates);
	BOOST_CHECK(static_cast<bool>(command->describe));
	const json::object& properties = command->args_schema.as_object().at("properties").as_object();
	for (const char* name : { "path", "name", "folder" }) {
		BOOST_TEST_CONTEXT(name) { BOOST_CHECK(properties.at(name).as_object().contains("description")); }
	}
}

BOOST_AUTO_TEST_CASE(APerson_WithOnlyAPath_GetsTheDialogAsBefore)
{
	const cmd::result result = call(cmd::origin::ui(), R"({"path":"/models/m.glb"})");
	BOOST_CHECK(result.ok);
	BOOST_TEST((dialog_imports == std::vector<std::string>{ "/models/m.glb" }));
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(AnAgent_GetsTheBackgroundImport_NeverTheDialog)
{
	// Asked for: the registry puts the question, then runs the call with its ticket.
	const std::optional<std::string> text = actions.commands().find("file.import_model")->describe({ json::parse(path_json()), agent });
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(source.string()) != std::string::npos);

	const cmd::result result = call(agent, path_json(), "ticket");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::int64_t job = result.value.as_object().at("job").as_int64();
	BOOST_CHECK_GT(job, 0);
	BOOST_CHECK(dialog_imports.empty());
	run_jobs();
	const auto info = jobs.info(static_cast<std::uint64_t>(job));
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::done);
	BOOST_CHECK(fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(AnAgent_WithoutAnApprovalOrWithOnlyAPath_ImportsNothing)
{
	// No ticket: denied, whatever was shown.
	BOOST_REQUIRE(actions.commands().find("file.import_model")->describe({ json::parse(path_json()), agent }).has_value());
	const cmd::result without_ticket = call(agent, path_json());
	BOOST_CHECK(!without_ticket.ok);
	BOOST_CHECK_EQUAL(without_ticket.code, cmd::codes::denied);

	// A ticket for a call nobody was asked about.
	const cmd::result not_shown = call(agent, json::serialize(json::object{ { "path", source.string() }, { "name", "other" } }), "ticket");
	BOOST_CHECK_EQUAL(not_shown.code, cmd::codes::denied);
	BOOST_CHECK(dialog_imports.empty());
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK_EQUAL(jobs.running_count(), 0u);
}

BOOST_AUTO_TEST_CASE(AnAgent_CannotUseTheDialogPathByNamingNothing)
{
	// The ui origin with only a path is the dialog; the same arguments from an agent are an import that needs approval.
	const cmd::result result = call(agent, path_json(), "ticket");
	BOOST_CHECK(!result.ok);
	BOOST_CHECK(dialog_imports.empty());
}

BOOST_AUTO_TEST_CASE(WithoutAnImporter_TheCommandIsDisabledWithTheReason)
{
	edt::action_registry bare_actions;
	edt::file_menu_hooks bare_hooks;
	edt::file_menu bare_menu(dialogs, bare_actions, bare_hooks);
	bare_menu.register_actions();
	const cmd::result result = [&] {
		cmd::call request;
		request.args = json::parse(path_json());
		request.from = agent;
		return bare_actions.invoke("file.import_model", request);
	}();
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::disabled);
	BOOST_CHECK(result.error.find("assimp") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
