#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_decode_runner.h"
#include <chrono>
#include <filesystem>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_shipping_dirs.h>
#include <fstream>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>
#endif

#if SNK_HAS_ASSIMP && defined(SNK_MODEL_DECODER_PATH) && defined(SNK_MODEL_DECODER_SELFTEST_PATH)

// The decoder process (#647): a model is read in a child that is killed when it hangs, runs over its memory or the
// import is cancelled, and whose scratch folder is gone on every path.

namespace {

namespace fs = std::filesystem;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool k_memory_limit_testable = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool k_memory_limit_testable = false;
#else
constexpr bool k_memory_limit_testable = true;
#endif
#else
constexpr bool k_memory_limit_testable = true;
#endif

constexpr const char* k_triangle_obj = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";

struct runner_fixture
{
	test_util::temp_dir dir{ "decode_runner" };

	edt::decode_runner_config config() const
	{
		edt::decode_runner_config result;
		result.decoder = SNK_MODEL_DECODER_PATH;
		return result;
	}

	// The test build of the decoder, which has the --self-test-* flags the shipped one does not.
	edt::decode_runner_config selftest_config() const
	{
		edt::decode_runner_config result = config();
		result.decoder = SNK_MODEL_DECODER_SELFTEST_PATH;
		return result;
	}

	edt::decode_request request_for(const fs::path& model, edt::decode_request::mode_kind mode) const
	{
		edt::decode_request request;
		request.mode = mode;
		request.source_path = model;
		return request;
	}

	static std::vector<std::byte> bytes_of(std::string_view text)
	{
		std::vector<std::byte> bytes(text.size());
		for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = static_cast<std::byte>(text[i]);
		return bytes;
	}

	// No run folder of this process is left: the runner makes one per run under the private scratch base of the user.
	bool scratch_is_empty() const
	{
		std::string error;
		const fs::path base = core::res::scratch_base_dir(error);
		if (base.empty()) return true;
		const std::string prefix = "snk-model_decoder-" + std::to_string(platform::current_process_id()) + "-";
		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
			if (entry.path().filename().string().starts_with(prefix)) return false;
		}
		return true;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDecodeRunnerTests, runner_fixture)

BOOST_AUTO_TEST_CASE(AScan_OfAnObj_ComesBackFromTheChild_AndTheFolderIsGone)
{
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const edt::decode_runner runner(config());
	const auto outcome = runner.run(request_for(model, edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {});
	BOOST_REQUIRE_MESSAGE(outcome.ok(), outcome.error);
	BOOST_CHECK_EQUAL(boost::json::value_to<std::string>(outcome.bundle->manifest.at("kind")), "scan");
	BOOST_CHECK(edt::model_file_scan_from_json(outcome.bundle->manifest.at("scan")).has_value());
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(AnImport_OfAnObj_ReturnsEveryCreatedResourceAsABlob)
{
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const edt::decode_runner runner(config());
	const auto outcome = runner.run(request_for(model, edt::decode_request::mode_kind::import), bytes_of(k_triangle_obj), {});
	BOOST_REQUIRE_MESSAGE(outcome.ok(), outcome.error);
	const auto imported = edt::import_intermediate_from_json(outcome.bundle->manifest.at("import"));
	BOOST_REQUIRE(imported.has_value());
	BOOST_CHECK_MESSAGE(imported->error.empty(), imported->error);
	BOOST_CHECK(!imported->created_tags.empty());
	BOOST_CHECK_EQUAL(outcome.bundle->blobs.size(), imported->created_tags.size());
	for (const edt::bundle_blob& blob : outcome.bundle->blobs) {
		BOOST_CHECK(!blob.bytes.empty());
	}
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(AChildThatRunsTooLong_IsKilledAtTheDeadline_AndTheFolderIsGone)
{
	auto cfg = selftest_config();
	cfg.timeout = std::chrono::milliseconds(400);
	cfg.extra_arguments = { "--self-test-sleep", "60000" };
	const edt::decode_runner runner(cfg);
	const auto started = std::chrono::steady_clock::now();
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {});
	BOOST_CHECK(!outcome.ok());
	BOOST_CHECK(outcome.timed_out);
	BOOST_CHECK_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(30));
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(ARequestToStop_KillsTheChild_AndTheFolderIsGone)
{
	auto cfg = selftest_config();
	cfg.extra_arguments = { "--self-test-sleep", "60000" };
	const edt::decode_runner runner(cfg);
	std::stop_source stop;
	std::jthread stopper([&stop] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		stop.request_stop();
	});
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), stop.get_token());
	BOOST_CHECK(!outcome.ok());
	BOOST_CHECK(outcome.cancelled);
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(AChildThatWantsMoreThanItsMemoryLimit_FailsAndTheEditorGoesOn)
{
	if (!k_memory_limit_testable) {
		BOOST_TEST_MESSAGE("skipped: a sanitizer build cannot limit the address space");
		return;
	}
	auto cfg = selftest_config();
	cfg.memory_limit_bytes = 256ull * 1024ull * 1024ull;
	cfg.extra_arguments = { "--self-test-alloc", std::to_string(2ull * 1024ull * 1024ull * 1024ull) };
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {});
	BOOST_CHECK(!outcome.ok());
	BOOST_CHECK(!outcome.timed_out);
	BOOST_CHECK(scratch_is_empty());
}

#if defined(__linux__)
namespace {

// Alive and not a zombie: a child whose parent died and whose reaper is slow still shows to kill(pid, 0).
bool is_running_process(std::int64_t pid)
{
	if (!platform::process_is_running(pid)) {
		return false;
	}
	std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
	std::string line;
	std::getline(stat, line);
	const std::size_t close = line.rfind(')');
	return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
}

}

BOOST_AUTO_TEST_CASE(AChild_DoesNotOutliveTheProcessThatStartedIt)
{
	const fs::path pid_file = dir.path() / "child.pid";
	const auto helper = platform::spawn_detached(SNK_MODEL_DECODER_SELFTEST_PATH, { "--self-test-spawn", pid_file.string() });
	BOOST_REQUIRE_MESSAGE(helper, helper.error);

	std::int64_t child_pid = 0;
	for (int i = 0; i < 200 && child_pid == 0; ++i) {
		std::ifstream in(pid_file);
		in >> child_pid;
		if (child_pid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	BOOST_REQUIRE(child_pid > 0);
	BOOST_REQUIRE(is_running_process(child_pid));

	::kill(static_cast<pid_t>(helper.process_id), SIGKILL);
	bool gone = false;
	for (int i = 0; i < 200 && !gone; ++i) {
		gone = !is_running_process(child_pid);
		if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	BOOST_CHECK_MESSAGE(gone, "the child of a killed parent still runs");
	if (!gone) ::kill(static_cast<pid_t>(child_pid), SIGKILL);
}
#endif

BOOST_AUTO_TEST_CASE(AMissingDecoder_IsAFailure_NotACrash)
{
	auto cfg = config();
	cfg.decoder = dir.path() / "no_such_decoder";
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {});
	BOOST_CHECK(!outcome.ok());
	BOOST_CHECK(!outcome.error.empty());
	BOOST_CHECK(scratch_is_empty());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(StaleFolders_OfADeadDecoderRun_AreRemoved_AndTheRestKept)
{
	std::string error;
	const fs::path base = core::res::scratch_base_dir(error);
	BOOST_REQUIRE_MESSAGE(!base.empty(), error);
	const std::string own = std::to_string(platform::current_process_id());
	const fs::path dead = base / "snk-model_decoder-2147483000-0123456789abcdef";
	const fs::path other_game = base / "snk-other_game_for_decode_test-2147483000-0123456789abcdef";
	const fs::path alive = base / ("snk-model_decoder-" + own + "-0123456789abcdef");
	for (const fs::path& folder : { dead, other_game, alive }) {
		fs::create_directories(folder);
		fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace);
	}
	BOOST_CHECK_GE(edt::decode_runner::remove_stale_folders(), 1u);
	BOOST_CHECK(!fs::exists(dead));
	BOOST_CHECK(fs::exists(other_game));
	BOOST_CHECK(fs::exists(alive));
	std::error_code ec;
	fs::remove_all(other_game, ec);
	fs::remove_all(alive, ec);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

#endif
