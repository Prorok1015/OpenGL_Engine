#include <boost/test/unit_test.hpp>
#include "test_folder_link.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_model_import.h"
#include "edt_decode_codec.h"
#include "edt_file_menu.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_job_service.h>
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/command/cmd_policy.h>
#include <snk/resource/res_asset_meta.h>
#include <boost/json/parse.hpp>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// The model import an agent asks for (#548): the file a person is shown is the file that is imported, nothing is
// written when the import fails or is cancelled, and nothing of the import stays in memory://. The model itself is
// faked (a few memory:// resources), so these tests need no assimp. An agent's source and its side files lie on a
// local disk only, and are looked at only after a person approved the call (#1380).

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

// A path as the question shows it (#991): '/' separators, escaped as the describe escapes it.
std::string shown_path(const fs::path& path)
{
	return text_escape::escaped_text(fs_paths::generic_utf8_of(path), 1024);
}

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
			scan_sandbox = source.sandbox;
			scan_rule = source.may_read;
			edt::model_file_scan scan;
			scan.error = scan_error;
			scan.files = listed_files;
			for (auto& file : scan.files) file.hashed = !scan_leaves_unhashed;
			for (const auto& file : listed_files) scan.total_bytes += file.size;
			scan.sandbox = scan_reports;
			return scan;
		};
		result.sandbox_status = [this] { return expected_sandbox; };
		result.resource_layers = [this] { return std::vector<fs::path>{ res_root }; };
		result.resources_path = [this] { return res_root; };
		result.blocker = [this]() -> std::optional<std::string> { return blocked; };
		result.now = [this] { return clock; };
		result.max_source_bytes = 4096;
		result.read_below = [this](const platform::beneath::read_request& request,
			const platform::beneath::read_limits& limits, std::vector<std::byte>& out) {
			const platform::beneath::read_result read = platform::beneath::read_beneath(request, limits, out);
			// The handle's stamp stays, the bytes differ: what only the content hash sees.
			if (flip_read_bytes && !request.stamp_only && !out.empty()) out.back() ^= std::byte{ 1 };
			return read;
		};
		result.drive_type = [this](char letter) {
			++drive_type_calls;
			return std::toupper(static_cast<unsigned char>(letter)) == remote_drive ? 4u : edt::k_drive_fixed;
		};
		result.mount_table = [this]() -> std::optional<std::string> {
			++mount_table_calls;
			return mounts;
		};
		return result;
	}

	// A path on a network volume as the classifier is faked for this platform.
	std::string remote_volume_path(std::string_view leaf)
	{
#if defined(_WIN32)
		remote_drive = 'Z';
		return "Z:\\models\\" + std::string(leaf);
#else
		mounts += "nas:/export /mnt/nas nfs4 rw 0 0\n";
		return "/mnt/nas/models/" + std::string(leaf);
#endif
	}

	// What assimp would leave in memory://: textures, a geometry and a prefab that refers to it.
	edt::import_intermediate fake_import(const edt::import_source& source, ds::task_context*)
	{
		const fs::path& model = source.path;
		++imports;
		import_sandbox = source.sandbox;
		seen_settings = source.settings;
		if (source.allowed) seen_allowed = *source.allowed;
		if (source.preloaded) seen_bytes.assign(reinterpret_cast<const char*>(source.preloaded->data()), source.preloaded->size());
		if (during_import) during_import();
		edt::import_intermediate imported;
		if (fail_import) {
			imported.error = "the model is broken";
			return imported;
		}
		const std::string stem = model.stem().string();
		const res::tag texture{ "memory://" + stem + "/textures/albedo.png" };
		const res::tag geometry{ "memory://" + stem + "/" + stem + ".geom.desc" };
		const res::tag prefab{ "memory://" + stem + "/" + stem + ".prefab.desc" };
		rs->store(texture, test_util::to_bytes("png bytes"));
		// One triangle in JSON arrays, as the decoder answers: the export writes it with its binary file (EPIC-79).
		std::string geometry_text = R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2],"vertices":[)";
		for (int i = 0; i < 36; ++i) geometry_text += i == 0 ? "0" : ",0";
		rs->store(geometry, test_util::to_bytes(geometry_text + "]}"));
		rs->store(prefab, test_util::to_bytes(R"({"geometry":")" + std::string(geometry.view()) + R"("})"));
		imported.all_tags = { texture, geometry, prefab };
		imported.created_tags = imported.all_tags;
		if (foreign_tag) imported.all_tags.push_back(*foreign_tag);
		imported.geom_tag = geometry;
		imported.prefab_tag = prefab;
		imported.source_path = model;
		imported.sandbox = import_reports;
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

	// The last step of a job runs on the main thread after its worker finishes; under ASan one drain can come before it.
	void run_jobs_until_settled(std::uint64_t job, std::chrono::milliseconds limit = std::chrono::seconds(10))
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		for (run_jobs(); jobs.is_running(job) && std::chrono::steady_clock::now() < deadline; run_jobs()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	edt::agent_import_request request(const std::string& name = {}, const std::string& folder = {}) const
	{
		return { source.string(), name, folder };
	}

	json::object finished(std::uint64_t job)
	{
		run_jobs_until_settled(job);
		const auto info = jobs.info(job);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
		BOOST_REQUIRE(info->result.has_value());
		return info->result->as_object();
	}

	// The question, the person's yes, the start: as the registry does it for an agent (the ticket is the registry's).
	edt::agent_model_import::started approved_start(const edt::agent_import_request& wanted, const cmd::origin& caller)
	{
		const auto shown = service->describe(wanted, caller);
		BOOST_REQUIRE(shown.has_value());
		return service->start(wanted, caller);
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
	edt::sandbox_mode scan_sandbox = edt::sandbox_mode::required;
	edt::sandbox_mode import_sandbox = edt::sandbox_mode::required;
	// What the import was told of the meshes (#1195).
	edt::model_import_settings seen_settings{};
	std::string scan_error;
	std::vector<edt::referenced_file> listed_files;
	bool scan_leaves_unhashed = false;
	std::vector<edt::referenced_file> seen_allowed;
	// The side-file rule the job handed the scan (#1408): set for an agent only.
	edt::model_read_rule scan_rule;
	std::string seen_bytes;
	std::optional<res::tag> foreign_tag;
	std::function<void()> during_import;
	bool flip_read_bytes = false;
	// What the fake decoder says of its OS sandbox (#1076), and what the editor expects before a run.
	edt::decoder_sandbox_status scan_reports{};
	edt::decoder_sandbox_status import_reports{};
	edt::decoder_sandbox_status expected_sandbox{};
	// The classifier of an agent's source: every drive a fixed disk but this one, a local root file system.
	int remote_drive = 0;
	std::string mounts = "/dev/sda1 / ext4 rw 0 0\n";
	int drive_type_calls = 0;
	int mount_table_calls = 0;
};

edt::decoder_sandbox_status sandbox_report(bool fs, std::string why)
{
	edt::decoder_sandbox_status reported;
	reported.reported = true;
	reported.fs = fs;
	reported.syscalls = true;
	reported.why = std::move(why);
	return reported;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentModelImportTests, import_fixture)

BOOST_AUTO_TEST_CASE(Resolve_NamesTheFileTheNameAndTheFilesItWillWrite)
{
	const edt::agent_import_plan plan = service->resolve(request(), false);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_CHECK(plan.source == source);
	BOOST_CHECK_EQUAL(plan.size, 22u);
	BOOST_CHECK_EQUAL(plan.name, "robot");
	BOOST_CHECK_EQUAL(plan.folder, "assets/robot");
	BOOST_CHECK_EQUAL(plan.prefab, "res://assets/robot/robot.prefab.desc");

	const edt::agent_import_plan named = service->resolve(request("hero", "res://models/heroes"), false);
	BOOST_REQUIRE_MESSAGE(named.ok(), named.error);
	BOOST_CHECK_EQUAL(named.prefab, "res://models/heroes/hero.prefab.desc");
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesWhatIsNoModelFileOnDisk)
{
	const auto code_of = [this](const edt::agent_import_request& wanted) {
		const edt::agent_import_plan plan = service->resolve(wanted, false);
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
	const edt::agent_import_plan big =
		service->resolve({ (dir.path() / "outside" / "big.glb").string(), "", "" }, false);
	BOOST_CHECK(!big.ok());
	BOOST_CHECK_EQUAL(big.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(big.error.find("exceeds 4 KiB for an agent's import (source)") != std::string::npos, big.error);
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
	const edt::agent_import_plan plan = service->resolve({ link.string(), "", "" }, false);
	BOOST_CHECK(!plan.ok());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK(plan.error.find("link") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesAnAssetNameThatIsTheExtensionOfAModelFormat)
{
	for (const char* name : { "obj", "fbx", "glb", "gltf", "OBJ", "Fbx" }) {
		const edt::agent_import_plan plan = service->resolve(request(name), false);
		BOOST_CHECK_MESSAGE(!plan.ok(), name);
		BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
		BOOST_CHECK_MESSAGE(plan.error.find("extension of a model format") != std::string::npos, plan.error);
	}
	BOOST_CHECK(service->resolve(request("objects"), false).ok());
}

BOOST_AUTO_TEST_CASE(Resolve_RefusesBadNamesAndFolders)
{
	for (const char* name : { "..", "a/b", "CON", "x.", "a:b", "a|b" }) {
		BOOST_TEST_CONTEXT("name " << name) {
			const edt::agent_import_plan plan = service->resolve(request(name), false);
			BOOST_CHECK(!plan.ok());
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
		}
	}
	for (const char* folder : { "../out", "/abs", "C:/x", "memory://x", "a//b", "a/./b", "a\\b", "nul/x", "a/b." }) {
		BOOST_TEST_CONTEXT("folder " << folder) {
			const edt::agent_import_plan plan = service->resolve(request("", folder), false);
			BOOST_CHECK(!plan.ok());
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
		}
	}
}

BOOST_AUTO_TEST_CASE(Resolve_AnExistingTargetNeedsInput_InAnyLayer)
{
	dir.write("res/assets/robot/robot.prefab.desc", "{}");
	const edt::agent_import_plan plan = service->resolve(request(), false);
	BOOST_CHECK(!plan.ok());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::needs_input);
	BOOST_CHECK(plan.error.find("res://assets/robot/robot.prefab.desc") != std::string::npos);
	BOOST_CHECK(service->resolve(request("other"), false).ok());
}

// #1361: the prefab's name is held by a .resource a deleted file left: the import asks before anything is parsed.
BOOST_AUTO_TEST_CASE(Resolve_APrefabNameALeftoverResourceHolds_NeedsInput)
{
	dir.write("res/assets/robot/robot.prefab.desc.resource", R"({"__type":"resource_meta","__version":1,)"
		R"("__guid":"5d8a1f3e-2b4c-4e6f-8a9b-0c1d2e3f4a5b"})");
	const edt::agent_import_plan plan = service->resolve(request(), false);
	BOOST_CHECK(!plan.ok());
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::needs_input);
	BOOST_CHECK(plan.error.find("robot.prefab.desc.resource") != std::string::npos);
	BOOST_CHECK(service->resolve(request("other"), false).ok());
}

BOOST_AUTO_TEST_CASE(Resolve_IsBusyWhileAnotherImportRunsOrTheEditorBlocks)
{
	blocked = "Stop Play to import.";
	const edt::agent_import_plan plan = service->resolve(request(), false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::busy);
	BOOST_CHECK_EQUAL(plan.error, "Stop Play to import.");
	blocked.reset();

	BOOST_REQUIRE(service->start(request(), cmd::origin::ui()).ok());
	BOOST_CHECK(service->running());
	BOOST_CHECK_EQUAL(service->resolve(request("second"), false).code, cmd::codes::busy);
	run_jobs();
	BOOST_CHECK(!service->running());
	BOOST_CHECK(service->resolve(request("second"), false).ok());
}

BOOST_AUTO_TEST_CASE(WithoutAnImporter_EverythingIsDisabledWithTheReason)
{
	edt::agent_import_options none = options();
	none.import = nullptr;
	edt::agent_model_import bare(exporter, jobs, *rs, std::move(none));
	BOOST_CHECK(!bare.available());
	const edt::agent_import_plan plan = bare.resolve(request(), false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::disabled);
	BOOST_CHECK(plan.error.find("assimp") != std::string::npos);
	const auto started = bare.start(request(), cmd::origin::ui());
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::disabled);
}

BOOST_AUTO_TEST_CASE(WithoutTheDecoder_EverythingIsDisabledWithAReasonThatNamesTheFile_AndNothingIsParsedHere)
{
	int parsed_here = 0;
	edt::agent_import_options none = options();
	none.import = [&parsed_here](const edt::import_source&, ds::task_context*) { ++parsed_here; return edt::import_intermediate{}; };
	none.scan = [&parsed_here](const edt::import_source&, ds::task_context*) { ++parsed_here; return edt::model_file_scan{}; };
	none.unavailable_reason = [] { return std::string("the model decoder 'bin/snk_model_decoder' was not found next to the editor"); };
	edt::agent_model_import bare(exporter, jobs, *rs, std::move(none));
	BOOST_CHECK(!bare.available());
	const edt::agent_import_plan plan = bare.resolve(request(), false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::disabled);
	BOOST_CHECK(plan.error.find("snk_model_decoder") != std::string::npos);
	const auto started = bare.start(request(), cmd::origin::ui());
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::disabled);
	BOOST_CHECK(started.error.find("snk_model_decoder") != std::string::npos);
	BOOST_CHECK_EQUAL(parsed_here, 0);
}

BOOST_AUTO_TEST_CASE(Describe_ShowsThePathTheDestination_AndForAnAgentNoSize)
{
	const std::optional<std::string> text = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(shown_path(source)) != std::string::npos);
	// An agent's question is made from the text: the size is read once a person approved.
	BOOST_CHECK(text->find("22 bytes") == std::string::npos);
	BOOST_CHECK(text->find("(its size is read once approved)") != std::string::npos);
	BOOST_CHECK(text->find("res://models/") != std::string::npos);
	BOOST_CHECK(text->find("'hero'") != std::string::npos);
	BOOST_CHECK(text->find("res://models/hero.prefab.desc") != std::string::npos);

	const std::optional<std::string> refused = service->describe({ "relative.glb", "", "" }, agent);
	BOOST_REQUIRE(refused.has_value());
	BOOST_CHECK(refused->rfind("This request will be refused", 0) == 0u);

	// A person's call is described from the disk.
	const std::optional<std::string> person = service->describe(request("hero", "models"), cmd::origin::ui());
	BOOST_REQUIRE(person.has_value());
	BOOST_CHECK(person->find("22 bytes") != std::string::npos);
}

// #991: the refusal escapes the path where it is built, and the question shows that escape once; an agent's question
// (made from the text) shows the path escaped once too.
BOOST_AUTO_TEST_CASE(Describe_ARefusalShowsAnEscapeOfThePathOnce)
{
	const std::string missing = fs_paths::generic_utf8_of(fs::weakly_canonical(dir.path() / "outside"))
		+ "/miss\xE2\x80\xAE" "ing.glb";
	const std::optional<std::string> asked = service->describe({ missing, "hero", "models" }, agent);
	BOOST_REQUIRE(asked.has_value());
	BOOST_TEST(asked->rfind("This request will be refused", 0) != 0u, *asked);
	BOOST_TEST(asked->find("/miss\\u{202E}ing.glb") != std::string::npos, *asked);
	BOOST_TEST(asked->find("\\\\u{202E}") == std::string::npos, *asked);
	BOOST_TEST(asked->find("\xE2\x80\xAE") == std::string::npos, *asked);

	const std::optional<std::string> refused = service->describe({ missing, "hero", "models" }, cmd::origin::ui());
	BOOST_REQUIRE(refused.has_value());
	BOOST_TEST(refused->rfind("This request will be refused", 0) == 0u, *refused);
	BOOST_TEST(refused->find("/miss\\u{202E}ing.glb") != std::string::npos, *refused);
	BOOST_TEST(refused->find("\\\\u{202E}") == std::string::npos, *refused);
	BOOST_TEST(refused->find("\xE2\x80\xAE") == std::string::npos, *refused);
}

#ifndef _WIN32
// #943: a file name may hold a line break (not on Windows); it must not become a line of the question.
BOOST_AUTO_TEST_CASE(Describe_AFileNameWithALineBreak_StaysOnOneLine)
{
	dir.write("outside/ro\n  harmless.glb", "glb bytes of the robot");
	const fs::path forged = fs::weakly_canonical(dir.path() / "outside") / "ro\n  harmless.glb";

	const std::optional<std::string> text = service->describe({ forged.string(), "hero", "models" }, agent);

	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->find("\n  harmless.glb") == std::string::npos);
	BOOST_TEST(text->find("ro\\n  harmless.glb") != std::string::npos);
}
#endif

BOOST_AUTO_TEST_CASE(Describe_ShowsOnlyThePathTheSizeAndTheDestination_AndParsesNothing)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const std::optional<std::string> text = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(shown_path(source)) != std::string::npos);
	BOOST_CHECK(text->find("skin.png") == std::string::npos);
	BOOST_CHECK(text->find("Nothing is copied into the project until") != std::string::npos);
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

// #1043: an agent's model is scanned and imported only by a decoder in its OS sandbox, whatever the editor's cfg says.
BOOST_AUTO_TEST_CASE(AnAgentsImport_AsksForNoSandboxOfItsOwn_ThePersonsSettingDecides)
{
	// Only a person sets the decoder's isolation (import/decoder_sandbox); an agent's import neither raises nor lowers it.
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_REQUIRE_EQUAL(scans, 1);
	BOOST_REQUIRE_EQUAL(imports, 1);
	BOOST_CHECK(scan_sandbox == edt::sandbox_mode::off);
	BOOST_CHECK(import_sandbox == edt::sandbox_mode::off);
}

// #1076: an agent's question says when the decoder runs without OS isolation, before it ran (the editor's expectation)
// and after its scan (what the decoder said); a whole sandbox adds nothing.
BOOST_AUTO_TEST_CASE(BothQuestions_SayWhenTheDecoderRunsWithoutOSIsolation)
{
	const std::optional<std::string> whole = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(whole.has_value());
	BOOST_CHECK(whole->find("isolation") == std::string::npos);

	expected_sandbox = sandbox_report(false, "the kernel has no usable Landlock");
	const std::optional<std::string> first = service->describe(request("hero", "models"), agent);
	BOOST_REQUIRE(first.has_value());
	BOOST_CHECK_MESSAGE(first->find("without OS isolation (the kernel has no usable Landlock)") != std::string::npos, *first);
	BOOST_CHECK_MESSAGE(first->find("every file") != std::string::npos, *first);

	expected_sandbox = {};
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	scan_reports = sandbox_report(false, "the OS sandbox is turned off (import/decoder_sandbox)");
	const json::object parked = finished(service->start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(parked.at("files_token").as_string());
	const std::optional<std::string> text = service->describe(second, agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK_MESSAGE(text->find("without OS isolation (the OS sandbox is turned off") != std::string::npos, *text);
}

// #1076: what the decoder said of its sandbox goes back to the agent with the job's result, never hidden.
BOOST_AUTO_TEST_CASE(TheJobsResult_CarriesWhatTheDecoderSaidOfItsSandbox)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	scan_reports = sandbox_report(false, "the kernel has no usable Landlock");
	const json::object parked = finished(approved_start(request("hero", "models"), agent).job);
	BOOST_REQUIRE(parked.contains("decoder_sandbox"));
	const json::object& listed = parked.at("decoder_sandbox").as_object();
	BOOST_CHECK_EQUAL(std::string(listed.at("fs").as_string()), "none");
	BOOST_CHECK_EQUAL(std::string(listed.at("syscalls").as_string()), std::string(edt::k_decoder_sandbox_syscalls_name));
	BOOST_CHECK_EQUAL(std::string(listed.at("why").as_string()), "the kernel has no usable Landlock");
	BOOST_CHECK(std::string(listed.at("warning").as_string()).find("every file") != std::string::npos);

	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(parked.at("files_token").as_string());
	import_reports = sandbox_report(true, {});
	const auto started = approved_start(second, agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	const json::object done = finished(started.job);
	BOOST_REQUIRE(done.contains("decoder_sandbox"));
	const json::object& imported = done.at("decoder_sandbox").as_object();
	BOOST_CHECK_EQUAL(std::string(imported.at("fs").as_string()), std::string(edt::k_decoder_sandbox_fs_name));
	BOOST_CHECK(!imported.contains("warning"));
	BOOST_CHECK(!imported.contains("why"));
}

// An importer that is no decoder process reports nothing, and nothing is added.
BOOST_AUTO_TEST_CASE(AnImporterThatReportsNoSandbox_AddsNothingToTheResult)
{
	const json::object done = finished(approved_start(request("hero", "models"), agent).job);
	BOOST_CHECK(!done.contains("decoder_sandbox"));
}

BOOST_AUTO_TEST_CASE(AModelWithFiles_EndsTheFirstJobWithTheirListAndWritesNothing)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 }, { dir.path() / "outside" / "mesh.bin", 3000 } };
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	const json::object result = finished(started.job);
	BOOST_CHECK(result.at("needs_files_approval").as_bool());
	BOOST_CHECK(!std::string(result.at("files_token").as_string()).empty());
	BOOST_CHECK_EQUAL(result.at("file_count").as_int64(), 2);
	BOOST_CHECK(!result.contains("files"));
	BOOST_CHECK(!result.contains("total_bytes"));
	BOOST_CHECK(json::serialize(result).find("skin.png") == std::string::npos);
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
	for (const auto& file : listed_files) BOOST_TEST_CONTEXT(file.path.string()) { BOOST_CHECK(text->find(shown_path(file.path)) != std::string::npos); }
	BOOST_CHECK(text->find("25 files") != std::string::npos);
	BOOST_CHECK(text->find("2500 bytes") != std::string::npos);

	const auto started = service->start(second, agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	scan_error = "must not be asked again";
	const json::object result = finished(started.job);
	BOOST_CHECK_EQUAL(std::string(result.at("prefab").as_string()), "res://models/hero.prefab.desc");
	BOOST_CHECK_EQUAL(scans, 1);
	BOOST_CHECK_EQUAL(imports, 1);
	BOOST_REQUIRE_EQUAL(seen_allowed.size(), 25u);
	BOOST_CHECK_EQUAL(seen_bytes, "glb bytes of the robot");
}

BOOST_AUTO_TEST_CASE(AListWithAFileThatWasNotHashed_IsNeverParked)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	scan_leaves_unhashed = true;
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK(info->error.find("hashed") != std::string::npos);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(TheSecondCall_WithoutAListOfItsOwn_IsRefused)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	const std::string token(first.at("files_token").as_string());

	edt::agent_import_request wrong_token = request("hero", "models");
	wrong_token.files_token = "files-999";
	BOOST_CHECK(service->describe(wrong_token, agent)->rfind("This request will be refused", 0) == 0u);
	BOOST_CHECK_EQUAL(service->start(wrong_token, agent).code, cmd::codes::denied);

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

BOOST_AUTO_TEST_CASE(OneSessionCannotPushAnotherSessionsQuestionOut)
{
	const cmd::origin other = cmd::origin::agent("session-2");
	BOOST_REQUIRE(service->describe(request("hero", "models"), agent).has_value());
	for (std::size_t i = 0; i <= edt::agent_model_import::k_max_shown + 2; ++i) {
		BOOST_REQUIRE(service->describe(request("n" + std::to_string(i), "models"), other).has_value());
	}
	const auto started = service->start(request("hero", "models"), agent);
	BOOST_CHECK_MESSAGE(started.ok(), started.error);
}

BOOST_AUTO_TEST_CASE(ADeniedSecondQuestion_LeavesNothingInTheProject)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = "whatever";
	BOOST_CHECK_EQUAL(service->start(second, agent).code, cmd::codes::denied);
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

// #1361: one asset of the import is a name a leftover .resource holds: the job fails naming it, nothing is written,
// and the .resource keeps its bytes (the new file would have taken the deleted asset's ID).
BOOST_AUTO_TEST_CASE(AnAssetNameALeftoverResourceHolds_FailsTheJob_AndWritesNothing)
{
	const std::string meta = R"({"__type":"resource_meta","__version":1,)"
		R"("__guid":"5d8a1f3e-2b4c-4e6f-8a9b-0c1d2e3f4a5b"})";
	dir.write("res/models/hero.geom.desc.resource", meta);
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs_until_settled(started.job);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("hero.geom.desc.resource") != std::string::npos, info->error);
	for (const char* name : { "hero.prefab.desc", "hero.geom.desc", "hero.geom.bin" }) {
		BOOST_TEST_CONTEXT(name) { BOOST_CHECK(!fs::exists(res_root / "models" / name)); }
	}
	BOOST_CHECK_EQUAL(read_file(res_root / "models" / "hero.geom.desc.resource"), meta);
}

BOOST_AUTO_TEST_CASE(Start_WithoutAnApproval_LooksAtNoPath)
{
	const auto missing = service->start({ (dir.path() / "no" / "such.glb").string(), "", "" }, agent);
	BOOST_CHECK_EQUAL(missing.code, cmd::codes::denied);
	const auto present = service->start(request(), agent);
	BOOST_CHECK_EQUAL(present.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(missing.error, present.error);
	BOOST_CHECK_EQUAL(scans, 0);
}

BOOST_AUTO_TEST_CASE(Job_ErasesOnlyTheMemoryCopiesItCreated)
{
	const res::tag foreign{ "memory://robot/textures/foreign.png" };
	rs->store(foreign, test_util::to_bytes("belongs to somebody else"));
	foreign_tag = foreign;
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE(started.ok());
	run_jobs();
	BOOST_CHECK(in_memory("memory://robot/textures/foreign.png"));
	BOOST_CHECK(!in_memory("memory://robot/robot.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(Start_AfterTheApproval_ImportsExportsAndCleansMemory)
{
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	BOOST_CHECK_GT(started.job, 0u);

	run_jobs_until_settled(started.job);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, info->error);
	BOOST_REQUIRE(info->result.has_value());
	const json::object& result = info->result->as_object();
	BOOST_CHECK_EQUAL(std::string(result.at("prefab").as_string()), "res://models/hero.prefab.desc");
	std::vector<std::string> assets;
	for (const json::value& asset : result.at("assets").as_array()) assets.emplace_back(asset.as_string());
	BOOST_TEST((assets == std::vector<std::string>{ "res://models/hero.geom.desc", "res://models/hero.prefab.desc", "res://models/textures/albedo.png" }));

	BOOST_CHECK(fs::exists(res_root / "models" / "hero.prefab.desc"));
	BOOST_CHECK(fs::exists(res_root / "models" / "hero.geom.desc"));
	BOOST_CHECK(fs::exists(res_root / "models" / "hero.geom.bin"));
	BOOST_CHECK_EQUAL(read_file(res_root / "models" / "textures" / "albedo.png"), "png bytes");
	// The memory:// references of the prefab point at the project files now.
	BOOST_CHECK(read_file(res_root / "models" / "hero.prefab.desc").find("res://models/hero.geom.desc") != std::string::npos);
	for (const char* tag : { "memory://robot/textures/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
	BOOST_CHECK(!service->running());
}

// #1195: the settings an agent asks for are shown to the person, reach the import and are recorded in the
// .resource of the prefab with the source's name and hash, never its path.
BOOST_AUTO_TEST_CASE(TheSettings_AreShown_ReachTheImport_AndAreRecordedInThePrefabsSidecar)
{
	edt::agent_import_request wanted = request("hero", "models");
	wanted.settings = json::parse(R"({"normals":"flat","tangents":false})");
	const std::optional<std::string> text = service->describe(wanted, agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK_MESSAGE(text->find("normals flat, tangents off") != std::string::npos, *text);
	const auto started = service->start(wanted, agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_CHECK(seen_settings.normals == edt::model_normals::flat);
	BOOST_CHECK(!seen_settings.tangents);

	const std::string sidecar = read_file(res_root / "models" / "hero.prefab.desc.resource");
	const res::asset_meta_result meta = res::read_asset_meta(sidecar);
	BOOST_REQUIRE(meta.meta.has_value());
	BOOST_CHECK_EQUAL(meta.meta->importer, "model");
	const edt::model_settings_parse settings = edt::parse_model_settings(meta.meta->settings);
	BOOST_REQUIRE_MESSAGE(settings.ok(), settings.error);
	BOOST_CHECK(settings.settings == seen_settings);
	BOOST_REQUIRE(settings.source.has_value());
	BOOST_CHECK_EQUAL(settings.source->name, "robot.glb");
	const std::string bytes = "glb bytes of the robot";
	const edt::content_digest digest = edt::digest_of(std::as_bytes(std::span(bytes)));
	BOOST_CHECK_EQUAL(settings.source->sha256, edt::digest_to_hex(digest));
	BOOST_CHECK(sidecar.find(dir.path().filename().string()) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(WithoutSettings_TheImportIsAsBefore_AndTheSidecarHoldsTheDefaults)
{
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_CHECK(seen_settings == edt::model_import_settings{});
	const fs::path sidecar = res_root / "models" / "hero.prefab.desc.resource";
	const res::asset_meta_result meta = res::read_asset_meta(read_file(sidecar));
	BOOST_REQUIRE(meta.meta.has_value());
	BOOST_CHECK(edt::parse_model_settings(meta.meta->settings).settings == edt::model_import_settings{});
}

BOOST_AUTO_TEST_CASE(BadSettings_AreRefusedBeforeAnythingIsLookedAt)
{
	const std::string with_source = R"({"source":{"name":"x.glb","sha256":")" + std::string(64, 'a') + R"("}})";
	for (const std::string& bad : { std::string(R"({"normals":"round"})"), std::string(R"({"scale":2})"),
			std::string(R"({"version":2})"), with_source }) {
		BOOST_TEST_CONTEXT(bad) {
			edt::agent_import_request wanted = request("hero", "models");
			wanted.settings = json::parse(bad);
			BOOST_CHECK(service->describe(wanted, agent)->rfind("This request will be refused", 0) == 0u);
			BOOST_CHECK_EQUAL(service->resolve(wanted, false).code, cmd::codes::invalid_args);
		}
	}
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

BOOST_AUTO_TEST_CASE(AFilesToken_HoldsForTheSettingsItWasMadeWith_Only)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	edt::agent_import_request first = request("hero", "models");
	first.settings = json::parse(R"({"normals":"flat"})");
	const json::object listed = finished(approved_start(first, agent).job);
	const std::string token(listed.at("files_token").as_string());

	edt::agent_import_request other = request("hero", "models");
	other.files_token = token;
	BOOST_CHECK(service->describe(other, agent)->rfind("This request will be refused", 0) == 0u);
	BOOST_CHECK_EQUAL(service->start(other, agent).code, cmd::codes::denied);

	// The same settings, spelled with a default added, are the same settings.
	edt::agent_import_request same = request("hero", "models");
	same.settings = json::parse(R"({"normals":"flat","tangents":true})");
	same.files_token = token;
	const std::optional<std::string> text = service->describe(same, agent);
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK_MESSAGE(text->rfind("This request will be refused", 0) != 0u, *text);
	const auto started = service->start(same, agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_CHECK(seen_settings.normals == edt::model_normals::flat);
}

BOOST_AUTO_TEST_CASE(Start_IsNotFreeForAnAgent_ItNeedsTheShownApproval)
{
	// Never shown: nothing starts.
	auto started = service->start(request(), agent);
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// Whether a person approved is the registry's to know (EdtImportModelActionTests): the service needs the plan
	// described for exactly this call.

	// Shown to another session.
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	started = service->start(request(), cmd::origin::agent("session-2"));
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);

	// The approval is used up by the start.
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	BOOST_REQUIRE(service->start(request(), agent).ok());
	run_jobs();
	started = service->start(request("again"), agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 1);
}

BOOST_AUTO_TEST_CASE(Start_RefusesAnApprovalThatIsTooOld)
{
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	clock += edt::agent_model_import::k_shown_lifetime + std::chrono::seconds(1);
	const auto started = service->start(request(), agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 0);
}

// An agent's first question showed no size: the start takes the file as it is then, after the source rule.
BOOST_AUTO_TEST_CASE(Start_ResolvesTheFileWhenItStarts_TheQuestionShowedNoSize)
{
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	dir.write("outside/robot.glb", "glb bytes of a much longer and different robot");
	const auto started = service->start(request(), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_CHECK_EQUAL(seen_bytes, "glb bytes of a much longer and different robot");
}

BOOST_AUTO_TEST_CASE(Start_RefusesAFileThatChangedSinceItsFilesWereListed)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(first.at("files_token").as_string());

	// Other bytes (the size).
	BOOST_REQUIRE(service->describe(second, agent).has_value());
	dir.write("outside/robot.glb", "glb bytes of a much longer and different robot");
	BOOST_CHECK_EQUAL(service->start(second, agent).code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(Start_RefusesAnotherFileAtTheSamePathSizeAndTime)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(first.at("files_token").as_string());
	BOOST_REQUIRE(service->describe(second, agent).has_value());
	const auto shown_time = fs::last_write_time(source);
	dir.write("outside/replacement.glb", "glb bytes of the robot");
	fs::last_write_time(dir.path() / "outside" / "replacement.glb", shown_time);
	fs::rename(dir.path() / "outside" / "replacement.glb", source);
	BOOST_REQUIRE_EQUAL(fs::last_write_time(source), shown_time);

	const auto started = service->start(second, agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(imports, 0);
}

// Same size and the write time set back: the change time of the handle's stamp (#1412) refuses the second call
// before any job, where only the content hash used to see it.
BOOST_AUTO_TEST_CASE(TheSecondCall_RefusesAModelWhoseBytesChangedSinceItsFilesWereListed)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(first.at("files_token").as_string());
	BOOST_REQUIRE(service->describe(second, agent).has_value());

	const auto time = fs::last_write_time(source);
	dir.write("outside/robot.glb", "glb bytes of the r0bot");
	fs::last_write_time(source, time);
	BOOST_REQUIRE_EQUAL(fs::file_size(source), std::string("glb bytes of the robot").size());

	const auto started = service->start(second, agent);
	BOOST_CHECK(!started.ok());
	BOOST_CHECK_EQUAL(started.code, cmd::codes::denied);
	BOOST_CHECK_MESSAGE(started.error.find("changed") != std::string::npos, started.error);
	run_jobs();
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

// The content hash of the second job, for bytes no stamp tells apart: a reader that gives other bytes from the same
// handle.
BOOST_AUTO_TEST_CASE(TheSecondJob_RefusesBytesOtherThanTheListedOnes_WhenTheStampIsTheSame)
{
	listed_files = { { dir.path() / "outside" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(first.at("files_token").as_string());
	BOOST_REQUIRE(service->describe(second, agent).has_value());

	flip_read_bytes = true;
	const auto started = service->start(second, agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("content") != std::string::npos, info->error);
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
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
	for (const char* tag : { "memory://robot/textures/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
		BOOST_TEST_CONTEXT(tag) { BOOST_CHECK(!in_memory(tag)); }
	}
}

// #1412: the same bytes' size written in place, the write time set back: the change time of the handle tells.
BOOST_AUTO_TEST_CASE(Job_RefusesASameSizeRewriteWithItsTimeSetBack)
{
	const auto started = approved_start(request(), agent);
	BOOST_REQUIRE(started.ok());
	during_import = [this] {
		const fs::file_time_type written = fs::last_write_time(source);
		dir.write("outside/robot.glb", "glb bytes of the r0bot");
		fs::last_write_time(source, written);
	};
	run_jobs();
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("changed") != std::string::npos, info->error);
	BOOST_CHECK(!fs::exists(res_root / "assets"));
}

// #1412: an agent's plan holds a stamp and no time or id: a plan with a stamp on one side only is another file.
BOOST_AUTO_TEST_CASE(SameFileAs_WithAStampOnOneSideOnly_IsAnotherFile)
{
	const edt::agent_import_plan plan = service->resolve(request(), false);
	BOOST_REQUIRE_MESSAGE(plan.ok(), plan.error);
	BOOST_REQUIRE(plan.stamp.has_value());
	BOOST_CHECK(plan.same_file_as(plan));
	edt::agent_import_plan bare = plan;
	bare.stamp.reset();
	BOOST_CHECK(!plan.same_file_as(bare));
	BOOST_CHECK(!bare.same_file_as(plan));
	edt::agent_import_plan touched = plan;
	touched.stamp->change_time_ns += 1;
	BOOST_CHECK(!plan.same_file_as(touched));
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
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "textures"));
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
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "textures" / "albedo.png"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.geom.desc"));
	// The binary file staged before the refused desc is taken back too.
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.geom.bin.new"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.geom.bin"));
	BOOST_CHECK(!fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
	for (const char* tag : { "memory://robot/textures/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
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
	for (const char* tag : { "memory://robot/textures/albedo.png", "memory://robot/robot.geom.desc", "memory://robot/robot.prefab.desc" }) {
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

BOOST_AUTO_TEST_CASE(Start_RunsOnlyThePlanOfItsOwnTicket)
{
	// Described for one question: a call without that ticket, or with another, uses the plan up and runs nothing.
	BOOST_REQUIRE(service->describe(request(), agent, "ticket-a").has_value());
	BOOST_CHECK_EQUAL(service->start(request(), agent).code, cmd::codes::denied);
	BOOST_REQUIRE(service->describe(request(), agent, "ticket-a").has_value());
	BOOST_CHECK_EQUAL(service->start(request(), agent, "ticket-b").code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(service->start(request(), agent, "ticket-a").code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(scans, 0);
	// Described without one (an allowed call) and run with one: refused too.
	BOOST_REQUIRE(service->describe(request(), agent).has_value());
	BOOST_CHECK_EQUAL(service->start(request(), agent, "ticket-a").code, cmd::codes::denied);

	BOOST_REQUIRE(service->describe(request(), agent, "ticket-a").has_value());
	const auto started = service->start(request(), agent, "ticket-a");
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	finished(started.job);
	BOOST_CHECK_EQUAL(imports, 1);
}

// A decoder's error that names a side file by its absolute path reaches the agent without this machine's folders.
BOOST_AUTO_TEST_CASE(AJobsError_NamesNoFolderOfTheSource)
{
	scan_error = "cannot read '" + fs_paths::generic_utf8_of(source.parent_path() / "textures" / "skin.png") + "' of "
		+ fs_paths::utf8_of(source);
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs_until_settled(started.job);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("<source folder>/textures/skin.png") != std::string::npos, info->error);
	BOOST_CHECK_MESSAGE(info->error.find("robot.glb") != std::string::npos, info->error);
	BOOST_CHECK_MESSAGE(info->error.find(fs_paths::utf8_of(source.parent_path())) == std::string::npos, info->error);
	BOOST_CHECK_MESSAGE(info->error.find("unit_test_tmp") == std::string::npos, info->error);
}

BOOST_AUTO_TEST_CASE(APersonsOwnCall_NeedsNoApproval)
{
	const auto started = service->start(request("mine", "models"), cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs();
	BOOST_CHECK_EQUAL(scans, 1);
	BOOST_CHECK(fs::exists(res_root / "models" / "mine.prefab.desc"));
}

// #1380: a share or a network volume is told from the text and the volume tables; the path is never looked at.
BOOST_AUTO_TEST_CASE(AnAgentsNetworkSource_IsRefusedBeforeAnyDiskAccess)
{
	for (const std::string& path : { std::string("\\\\server\\share\\x.glb"), std::string("//server/share/x.glb"),
		std::string("\\\\?\\UNC\\server\\share\\x.glb") }) {
		BOOST_TEST_CONTEXT(path)
		{
			const edt::agent_import_request wanted{ path, "hero", "models" };
			const std::optional<std::string> question = service->describe(wanted, agent);
			BOOST_REQUIRE(question.has_value());
			BOOST_CHECK_MESSAGE(question->rfind("This request will be refused", 0) == 0u, *question);
			BOOST_CHECK_MESSAGE(question->find("network share or a device path") != std::string::npos, *question);
			const edt::agent_import_plan plan = service->resolve(wanted, false);
			BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
			BOOST_CHECK_MESSAGE(plan.error.find("no such file") == std::string::npos, plan.error);
			BOOST_CHECK_EQUAL(service->start(wanted, agent).code, cmd::codes::denied);
		}
	}
	// Told from the text alone: neither the drive table nor the mount table was asked.
	BOOST_CHECK_EQUAL(drive_type_calls, 0);
	BOOST_CHECK_EQUAL(mount_table_calls, 0);

	// A mapped network drive (Windows) or a network mount (POSIX): the classifier says so.
	const edt::agent_import_request remote{ remote_volume_path("x.glb"), "hero", "models" };
	const std::optional<std::string> question = service->describe(remote, agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->rfind("This request will be refused", 0) == 0u, *question);
	BOOST_CHECK_MESSAGE(question->find("network") != std::string::npos, *question);
	const edt::agent_import_plan plan = service->resolve(remote, false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(plan.error.find("no such file") == std::string::npos, plan.error);
	BOOST_CHECK_GT(drive_type_calls + mount_table_calls, 0);
	BOOST_CHECK_EQUAL(service->start(remote, agent).code, cmd::codes::denied);
	// A person may pick a file on a share: the rule is not theirs (the file is simply missing here).
	BOOST_CHECK_EQUAL(service->resolve(remote, true).code, cmd::codes::not_found);
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(AnAgentsQuestion_DoesNotLookAtTheSource)
{
	// A file that is not there: the question is made from the text, the start (after approval) finds it missing.
	const edt::agent_import_request missing{ (dir.path() / "outside" / "missing.glb").string(), "hero", "models" };
	const std::optional<std::string> question = service->describe(missing, agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->find("This request will be refused") == std::string::npos, *question);
	BOOST_CHECK_MESSAGE(question->find("no such file") == std::string::npos, *question);
	BOOST_CHECK_MESSAGE(question->find("approved") != std::string::npos, *question);
	const auto started = service->start(missing, agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::not_found);
	// What the agent reads names the file, not this machine's folders.
	BOOST_CHECK_MESSAGE(started.error.find("missing.glb") != std::string::npos, started.error);
	BOOST_CHECK_MESSAGE(started.error.find(dir.path().generic_string()) == std::string::npos, started.error);
	BOOST_CHECK_MESSAGE(started.error.find(dir.path().string()) == std::string::npos, started.error);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtAgentModelImportSideFileTests, import_fixture)

BOOST_AUTO_TEST_CASE(ASideFileThatTheFirstJobListsOnANetworkVolume_FailsThatJob)
{
	listed_files = { { fs_paths::path_from_utf8(remote_volume_path("skin.png")), 1000 } };
	const auto started = approved_start(request("hero", "models"), agent);
	BOOST_REQUIRE_MESSAGE(started.ok(), started.error);
	run_jobs_until_settled(started.job);
	const auto info = jobs.info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_CHECK(info->status == edt::job_status::failed);
	BOOST_CHECK_MESSAGE(info->error.find("an agent may not read") != std::string::npos, info->error);
	BOOST_CHECK_EQUAL(imports, 0);
}

BOOST_AUTO_TEST_CASE(ASideFileOnAVolumeThatTurnedRemote_IsRefusedByTheSecondCallBeforeItIsRead)
{
	listed_files = { { dir.path() / "outside" / "textures" / "skin.png", 1000 } };
	const json::object first = finished(approved_start(request("hero", "models"), agent).job);
	edt::agent_import_request second = request("hero", "models");
	second.files_token = std::string(first.at("files_token").as_string());
	const std::string local_mounts = mounts;
	// The side file's folder is a network volume now (a share mounted there, or its drive mapped to one).
	const auto turn_remote = [&] {
#if defined(_WIN32)
		remote_drive = std::toupper(static_cast<unsigned char>(fs_paths::utf8_of(dir.path()).front()));
#else
		mounts += "nas:/export " + fs_paths::generic_utf8_of(dir.path() / "outside" / "textures") + " nfs4 rw 0 0\n";
#endif
	};

	// Before the question: the question says the call will be refused, and nothing was remembered for it.
	turn_remote();
	const std::optional<std::string> question = service->describe(second, agent);
	BOOST_REQUIRE(question.has_value());
	BOOST_CHECK_MESSAGE(question->rfind("This request will be refused", 0) == 0u, *question);
#if !defined(_WIN32)
	BOOST_CHECK_MESSAGE(question->find("refers to a file an agent may not read") != std::string::npos, *question);
#endif
	BOOST_CHECK_EQUAL(service->start(second, agent).code, cmd::codes::denied);

	// After the question, before the start: the start refuses before a job reads it.
	mounts = local_mounts;
	remote_drive = 0;
	BOOST_REQUIRE(service->describe(second, agent).has_value());
	turn_remote();
	const auto started = service->start(second, agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::invalid_args);
#if !defined(_WIN32)
	// On Windows the source shares the drive and is refused first.
	BOOST_CHECK_MESSAGE(started.error.find("an agent may not read") != std::string::npos, started.error);
#endif
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
}

// #1408: a link on the way to the source is refused where it is, never followed (here its target is gone).
BOOST_AUTO_TEST_CASE(AnAgentsSource_BelowAFolderLink_IsRefusedAtTheLink_BeforeAnythingFollowsIt)
{
	const fs::path outside = source.parent_path();
	fs::create_directories(outside / "gone");
	test_util::make_folder_link(outside / "linked", outside / "gone");
	fs::remove(outside / "gone");
	const edt::agent_import_request wanted{ (outside / "linked" / "robot.glb").string(), "hero", "models" };

	const edt::agent_import_plan plan = service->resolve(wanted, false);
	BOOST_CHECK_EQUAL(plan.code, cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(plan.error.find("'linked' on the way to the source is a link") != std::string::npos, plan.error);
	BOOST_CHECK_MESSAGE(plan.error.find("no such file") == std::string::npos, plan.error);

	const auto started = approved_start(wanted, agent);
	BOOST_CHECK_EQUAL(started.code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK(!fs::exists(res_root / "models"));
	// A person's import has no such rule: the file is simply not there.
	BOOST_CHECK_EQUAL(service->resolve(wanted, true).code, cmd::codes::not_found);
}

BOOST_AUTO_TEST_CASE(AFolderLinkInsideTheModelFolder_IsRefusedToAnAgentsScan_APersonsReadsIt)
{
	const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'x');
	dir.write("outside/tex/skin.png", png);
	test_util::make_folder_link(source.parent_path() / "linked", source.parent_path() / "tex");

	BOOST_REQUIRE(finished(approved_start(request("hero", "models"), agent).job).contains("prefab"));
	BOOST_REQUIRE(scan_rule);
	edt::model_file_access as_agent(source, {}, nullptr, {}, true, scan_rule);
	const auto refused = as_agent.resolve("linked/skin.png", edt::model_file_kind::texture);
	BOOST_CHECK(refused.refusal == edt::refusal_kind::rule);
	BOOST_CHECK_MESSAGE(refused.error.find("'linked' on the way to the source is a link") != std::string::npos,
		refused.error);
	BOOST_CHECK(as_agent.files().empty());
	BOOST_CHECK(as_agent.resolve("tex/skin.png", edt::model_file_kind::texture).found());

	const auto mine = service->start(request("mine", "mine"), cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(mine.ok(), mine.error);
	finished(mine.job);
	BOOST_CHECK(!scan_rule);
	edt::model_file_access as_person(source, {}, nullptr, {}, true, scan_rule);
	const auto found = as_person.resolve("linked/skin.png", edt::model_file_kind::texture);
	BOOST_REQUIRE_MESSAGE(found.found(), found.error);
	BOOST_CHECK(as_person.read(found).has_value());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(ANetworkMountBelowTheModelFolder_IsRefusedToAnAgentsScan)
{
	const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'x');
	dir.write("outside/net/skin.png", png);
	BOOST_REQUIRE(finished(approved_start(request("hero", "models"), agent).job).contains("prefab"));
	BOOST_REQUIRE(scan_rule);
	mounts += "nas:/export " + fs_paths::generic_utf8_of(source.parent_path() / "net") + " nfs rw 0 0\n";
	edt::model_file_access as_agent(source, {}, nullptr, {}, true, scan_rule);
	const auto refused = as_agent.resolve("net/skin.png", edt::model_file_kind::texture);
	BOOST_CHECK(refused.refusal == edt::refusal_kind::rule);
	BOOST_CHECK_MESSAGE(refused.error.find("'nfs' file system") != std::string::npos, refused.error);
	BOOST_CHECK(as_agent.files().empty());
}
#endif

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

// A policy that says the same to every call, and a person who can be asked.
struct fixed_gate final : cmd::policy_gate
{
	explicit fixed_gate(cmd::policy_mode answer) : mode(answer) {}
	cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override
	{
		return { mode, "test policy", cmd::agent_category::unset };
	}
	bool can_ask() const override { return true; }
	cmd::policy_mode mode;
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
	BOOST_CHECK(command->describe_before_allowed_invoke);
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
	// Asked for: the registry puts the question (described with its ticket), then runs the call with that ticket.
	const cmd::command_desc* command = actions.commands().find("file.import_model");
	const std::optional<std::string> text = command->describe({ json::parse(path_json()), agent, 0, "ticket" });
	BOOST_REQUIRE(text.has_value());
	BOOST_CHECK(text->find(shown_path(source)) != std::string::npos);

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

BOOST_AUTO_TEST_CASE(SettingsWithAKeyTheSchemaDoesNotName_AreRefused)
{
	json::object args{ { "path", source.string() }, { "settings", json::object{ { "scale", 2 } } } };
	const cmd::result result = call(cmd::origin::ui(), json::serialize(args));
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, cmd::codes::invalid_args);
	BOOST_CHECK(dialog_imports.empty());
	const json::object& properties = actions.commands().find("file.import_model")->args_schema.as_object()
		.at("properties").as_object();
	BOOST_CHECK(!properties.at("settings").as_object().at("additionalProperties").as_bool());
}

BOOST_AUTO_TEST_CASE(APerson_WithSettings_GetsTheBackgroundImport_NotTheDialog)
{
	json::object args{ { "path", source.string() }, { "settings", json::object{ { "normals", "flat" } } } };
	const cmd::result result = call(cmd::origin::ui(), json::serialize(args));
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK(dialog_imports.empty());
	run_jobs_until_settled(static_cast<std::uint64_t>(result.value.as_object().at("job").as_int64()));
	BOOST_CHECK(seen_settings.normals == edt::model_normals::flat);
}

BOOST_AUTO_TEST_CASE(AnAgentCall_WithoutDescribe_IsRefused)
{
	// No question was put and the registry described nothing (no gate judges here): the import refuses it, with or
	// without a ticket, before the source is looked at.
	const cmd::result without_ticket = call(agent, path_json());
	BOOST_CHECK(!without_ticket.ok);
	BOOST_CHECK_EQUAL(without_ticket.code, cmd::codes::denied);
	const cmd::result test_call = call(cmd::origin::test(), path_json());
	BOOST_CHECK_EQUAL(test_call.code, cmd::codes::denied);

	// A ticket for a call nobody was asked about.
	const cmd::result not_shown = call(agent, json::serialize(json::object{ { "path", source.string() }, { "name", "other" } }), "ticket");
	BOOST_CHECK_EQUAL(not_shown.code, cmd::codes::denied);
	BOOST_CHECK(dialog_imports.empty());
	BOOST_CHECK_EQUAL(imports, 0);
	BOOST_CHECK_EQUAL(jobs.running_count(), 0u);
}

BOOST_AUTO_TEST_CASE(AnAgentsCallThePolicyAllows_IsDescribedByTheRegistry_AndRuns)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::allow));
	const cmd::result result = call(agent, path_json());
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	run_jobs_until_settled(static_cast<std::uint64_t>(result.value.as_object().at("job").as_int64()));
	BOOST_CHECK(fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
	BOOST_CHECK(dialog_imports.empty());
	actions.commands().set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AnAgentsCall_WithATicketNotApprovedOrRejected_IsDenied_AnApprovedOneRuns)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::ask));
	const cmd::result asked = call(agent, path_json());
	BOOST_REQUIRE_EQUAL(asked.code, cmd::codes::confirmation_required);
	const std::string ticket(asked.value.at("ticket").as_string());
	BOOST_CHECK_EQUAL(call(agent, path_json(), ticket).code, cmd::codes::denied);

	actions.commands().discard(ticket);
	BOOST_CHECK_EQUAL(call(agent, path_json(), ticket).code, cmd::codes::denied);
	run_jobs();
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK(!fs::exists(res_root / "assets"));

	const cmd::result again = call(agent, path_json());
	BOOST_REQUIRE_EQUAL(again.code, cmd::codes::confirmation_required);
	const std::string approved(again.value.at("ticket").as_string());
	BOOST_REQUIRE(actions.commands().approve(approved));
	const cmd::result ran = call(agent, path_json(), approved);
	BOOST_REQUIRE_MESSAGE(ran.ok, ran.error);
	run_jobs_until_settled(static_cast<std::uint64_t>(ran.value.as_object().at("job").as_int64()));
	BOOST_CHECK(fs::exists(res_root / "assets" / "robot" / "robot.prefab.desc"));
	actions.commands().set_policy_gate(nullptr);
}

// The plan is bound to the question it was made for: a call with another ticket, or none, after the person rejected it
// runs nothing; the source was never looked at.
BOOST_AUTO_TEST_CASE(APlanDescribedForARejectedTicket_RunsForNoOtherCall)
{
	actions.commands().set_policy_gate(std::make_shared<fixed_gate>(cmd::policy_mode::ask));
	const cmd::result asked = call(agent, path_json());
	BOOST_REQUIRE_EQUAL(asked.code, cmd::codes::confirmation_required);
	actions.commands().discard(std::string(asked.value.at("ticket").as_string()));
	// No gate judges these calls: the import alone holds the line.
	actions.commands().set_policy_gate(nullptr);
	BOOST_CHECK_EQUAL(call(agent, path_json()).code, cmd::codes::denied);
	BOOST_CHECK_EQUAL(call(agent, path_json(), "another-ticket").code, cmd::codes::denied);
	run_jobs();
	BOOST_CHECK_EQUAL(scans, 0);
	BOOST_CHECK(!fs::exists(res_root / "assets"));
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
