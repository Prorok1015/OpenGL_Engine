#include <boost/test/unit_test.hpp>
#include "test_decoder_sandbox.hpp"
#include "test_temp_dir.hpp"
#include "edt_decode_runner.h"
#include "edt_decoder_copy.h"
#if defined(_WIN32)
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif
#include <chrono>
#include <cstdint>
#include <ios>
#include <filesystem>
#include <memory>
#include <snk/platform/plt_process.h>
#include <snk/resource/res_shipping_dirs.h>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>
#endif

#if SNK_HAS_ASSIMP

#if !defined(SNK_MODEL_DECODER_PATH) || !defined(SNK_MODEL_DECODER_SELFTEST_PATH)
#error "the decoder process tests need the decoder paths from CMake; an empty suite would hide a decoder that is not built"
#endif

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
	const auto outcome = runner.run(request_for(model, edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {}, {});
	BOOST_REQUIRE_MESSAGE(outcome.ok(), outcome.error);
	BOOST_CHECK_EQUAL(boost::json::value_to<std::string>(outcome.bundle->manifest.at("kind")), "scan");
	const auto requests = edt::scan_requests_from_json(outcome.bundle->manifest);
	BOOST_REQUIRE(requests.has_value());
	BOOST_CHECK_MESSAGE(requests->error.empty(), requests->error);
	BOOST_CHECK(requests->requested.empty());
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(AnImport_OfAnObj_ReturnsEveryCreatedResourceAsABlob)
{
	const fs::path model = dir.write("m/tri.obj", k_triangle_obj);
	const edt::decode_runner runner(config());
	const auto outcome = runner.run(request_for(model, edt::decode_request::mode_kind::import), bytes_of(k_triangle_obj), {}, {});
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
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {}, {});
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
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {}, stop.get_token());
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
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {}, {});
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

// #1043: a child that cannot have the sandbox the run requires ends with exit code 4 before it reads the request; the
// runner says why, from the child's own line.
BOOST_AUTO_TEST_CASE(AChildThatCannotHaveTheRequiredSandbox_IsRefused_WithItsReason)
{
	auto cfg = selftest_config();
	cfg.sandbox = edt::sandbox_mode::required;
	cfg.extra_arguments = { "--self-test-pretend-no-landlock" };
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {}, {});
	BOOST_CHECK(!outcome.ok());
	BOOST_CHECK(outcome.sandbox_refused);
	BOOST_CHECK_MESSAGE(outcome.error.starts_with("the model decoder cannot run in its sandbox: "), outcome.error);
#if defined(_WIN32)
	// The runner alone does not confine (process_decoder does, #1054): the child is in no AppContainer.
	BOOST_CHECK_MESSAGE(outcome.error.find("AppContainer") != std::string::npos, outcome.error);
#else
	BOOST_CHECK_MESSAGE(outcome.error.find("Landlock") != std::string::npos, outcome.error);
#endif
	BOOST_CHECK(scratch_is_empty());
}

#if defined(__linux__)
namespace {

struct probe_result
{
	int exit_code = -1;
	std::string out;
	std::string err;
};

// The test build of the decoder run by hand in a run folder of its own, with the sandbox flags and a self-test probe.
probe_result run_probe(const fs::path& scratch, std::vector<std::string> arguments)
{
	fs::create_directories(scratch);
	fs::permissions(scratch, fs::perms::owner_all, fs::perm_options::replace);
	platform::process_options options;
	options.working_dir = scratch;
	probe_result result;
	options.on_stderr_line = [&result](std::string_view line) { result.err += std::string(line) + "\n"; };
	const auto run = platform::run_process(SNK_MODEL_DECODER_SELFTEST_PATH, arguments, options,
		[&result](std::string_view line) { result.out += std::string(line) + "\n"; }, {});
	result.exit_code = run.exit_code;
	return result;
}

}

BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotOpenAFileOutsideItsFolder_ConnectOrStartAProgram)
{
	if (test_util::skipped_without_landlock()) return;
	const int abi = test_util::landlock_abi();
	const fs::path scratch = dir.path() / "scratch";
	const fs::path outside = dir.write("outside.txt", "a secret");
	const fs::path inside = dir.write("scratch/inside.txt", "the child's own");

	const auto opened_outside = run_probe(scratch, { "--sandbox", "required", "--self-test-open", outside.string() });
	BOOST_CHECK_MESSAGE(opened_outside.exit_code == 0, "open outside: " << opened_outside.exit_code << " " << opened_outside.err);
	// The probe itself works: what lies in its own folder opens.
	const auto opened_inside = run_probe(scratch, { "--sandbox", "required", "--self-test-open", inside.string() });
	BOOST_CHECK_MESSAGE(opened_inside.exit_code == 5, "open inside: " << opened_inside.exit_code << " " << opened_inside.err);

	// Without the filter (a sanitizer build) only Landlock refuses TCP, from ABI 4.
	if (test_util::k_decoder_has_syscall_filter || abi >= 4) {
		const auto connected = run_probe(scratch, { "--sandbox", "required", "--self-test-connect" });
		BOOST_CHECK_MESSAGE(connected.exit_code == 0, "connect: " << connected.exit_code << " " << connected.err);
	}
	const auto executed = run_probe(scratch, { "--sandbox", "required", "--self-test-exec" });
	BOOST_CHECK_MESSAGE(executed.exit_code == 0, "exec: " << executed.exit_code << " " << executed.err);
}

// #1099: a hard link to a file outside would give the child its contents and a way to write them.
BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotHardLinkAFileFromOutsideIntoItsFolder)
{
	if (test_util::skipped_without_landlock()) return;
	const fs::path outside = dir.write("outside.txt", "a secret");
	const auto linked = run_probe(dir.path() / "scratch", { "--sandbox", "required", "--self-test-hardlink", outside.string() });
	BOOST_CHECK_MESSAGE(linked.exit_code == 0, "required: " << linked.exit_code << " " << linked.err);
	const auto unsandboxed = run_probe(dir.path() / "scratch_off", { "--sandbox", "off", "--self-test-hardlink", outside.string() });
	BOOST_CHECK_MESSAGE(unsandboxed.exit_code == 5, "off: " << unsandboxed.exit_code << " " << unsandboxed.err);
}

BOOST_AUTO_TEST_CASE(TheSandboxReport_UnderRequired_SaysLandlock_AndSeccompOutsideSanitizerBuilds)
{
	if (test_util::skipped_without_landlock()) return;
	const auto report = run_probe(dir.path() / "scratch", { "--sandbox", "required", "--self-test-sandbox-report" });
	BOOST_REQUIRE_MESSAGE(report.exit_code == 0, report.exit_code << " " << report.err);
	const std::string expected = test_util::k_decoder_has_syscall_filter ? "fs=landlock syscalls=seccomp\n" : "fs=landlock syscalls=none\n";
	BOOST_CHECK_EQUAL(report.out, expected);
}

BOOST_AUTO_TEST_CASE(WithoutLandlock_RequiredRefusesToRun_AndBestEffortRunsWithoutIt)
{
	const fs::path scratch = dir.path() / "scratch";
	const auto required = run_probe(scratch, { "--sandbox", "required", "--self-test-pretend-no-landlock", "--self-test-sandbox-report" });
	BOOST_CHECK_EQUAL(required.exit_code, edt::k_decoder_sandbox_exit_code);
	BOOST_CHECK_MESSAGE(required.err.starts_with(std::string(edt::k_decoder_sandbox_refusal_prefix)), required.err);
	BOOST_CHECK(required.out.empty());

	const auto best_effort = run_probe(scratch, { "--sandbox", "best_effort", "--self-test-pretend-no-landlock", "--self-test-sandbox-report" });
	BOOST_CHECK_EQUAL(best_effort.exit_code, 0);
	BOOST_CHECK_MESSAGE(best_effort.out.starts_with("fs=none "), best_effort.out);
}

BOOST_AUTO_TEST_CASE(ASandboxThePersonTurnedOff_IsNotSetUp_EvenWhereTheSystemHasOne)
{
	const fs::path scratch = dir.path() / "scratch";
	const auto off = run_probe(scratch, { "--sandbox", "off", "--self-test-sandbox-report" });
	BOOST_CHECK_EQUAL(off.exit_code, 0);
	BOOST_CHECK_EQUAL(off.out, "fs=none syscalls=none\n");
}

// Fable's review of #1078: fcntl could make the kernel signal the editor (F_SETOWN + O_ASYNC, F_SETSIG, F_NOTIFY).
BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotSignalTheEditor_NorMakeTheKernelDoItForIt)
{
	const fs::path scratch = dir.path() / "scratch";
	// The probe works: without the sandbox each of these goes through.
	const auto unsandboxed = run_probe(scratch, { "--sandbox", "off", "--self-test-signal" });
	BOOST_CHECK_MESSAGE(unsandboxed.exit_code == 5, "off: " << unsandboxed.exit_code << " " << unsandboxed.err);
	if (!test_util::k_decoder_has_syscall_filter) {
		BOOST_TEST_MESSAGE("skipped: a sanitizer build has no system call filter");
		return;
	}
	const auto sandboxed = run_probe(scratch, { "--sandbox", "best_effort", "--self-test-signal" });
	BOOST_CHECK_MESSAGE(sandboxed.exit_code == 0, "best_effort: " << sandboxed.exit_code << " " << sandboxed.err);
}

// Landlock covers only the thread that applies it: a decoder with a second thread before its sandbox must not run.
BOOST_AUTO_TEST_CASE(AChildWithASecondThreadBeforeItsSandbox_RefusesToRun_EvenInBestEffort)
{
	if (!test_util::k_decoder_has_syscall_filter) {
		BOOST_TEST_MESSAGE("skipped: a sanitizer build may have a thread of its runtime and does not count them");
		return;
	}
	const auto threaded = run_probe(dir.path() / "scratch", { "--sandbox", "best_effort", "--self-test-extra-thread" });
	BOOST_CHECK_EQUAL(threaded.exit_code, edt::k_decoder_sandbox_exit_code);
	BOOST_CHECK_MESSAGE(threaded.err.find("threads") != std::string::npos, threaded.err);
}

namespace {

// #1082: one call the sandbox must refuse, run by the child once it is sandboxed.
probe_result run_call(const fs::path& scratch, const char* sandbox, const char* call, const fs::path& path = {})
{
	std::vector<std::string> arguments{ "--sandbox", sandbox, "--self-test-call", call };
	if (!path.empty()) arguments.insert(arguments.end(), { "--self-test-path", path.string() });
	return run_probe(scratch, std::move(arguments));
}

}

BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotWriteOutsideItsFolder)
{
	if (test_util::skipped_without_landlock()) return;
	const fs::path scratch = dir.path() / "scratch";
	const fs::path outside = dir.path() / "written_outside.txt";
	const auto refused = run_call(scratch, "required", "write", outside);
	BOOST_CHECK_MESSAGE(refused.exit_code == 0, "write outside: " << refused.exit_code << " " << refused.err);
	BOOST_CHECK(!fs::exists(outside));
	// The probe itself works: its own folder takes the file.
	const auto written = run_call(scratch, "required", "write", scratch / "written_inside.txt");
	BOOST_CHECK_MESSAGE(written.exit_code == 5, "write inside: " << written.exit_code << " " << written.err);
}

BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotTruncateOrChmodAFileOutsideItsFolder)
{
	if (test_util::skipped_without_landlock()) return;
	const fs::path scratch = dir.path() / "scratch";
	// The probes work where nothing stops them; they are tried on a file of their own, not on the one guarded below.
	const fs::path control = dir.write("control.txt", "x");
	const auto truncated = run_call(scratch, "off", "truncate", control);
	BOOST_CHECK_MESSAGE(truncated.exit_code == 5, "truncate, off: " << truncated.exit_code << " " << truncated.err);
	const auto chmodded = run_call(scratch, "off", "chmod", control);
	BOOST_CHECK_MESSAGE(chmodded.exit_code == 5, "chmod, off: " << chmodded.exit_code << " " << chmodded.err);

	const fs::path victim = dir.write("victim.txt", "keep me");
	const fs::perms before = fs::status(victim).permissions();
	// Landlock refuses a truncate from ABI 3; the filter refuses truncate(2) whatever the ABI.
	if (test_util::k_decoder_has_syscall_filter || test_util::landlock_abi() >= 3) {
		const auto refused = run_call(scratch, "required", "truncate", victim);
		BOOST_CHECK_MESSAGE(refused.exit_code == 0, "truncate: " << refused.exit_code << " " << refused.err);
		BOOST_CHECK_EQUAL(fs::file_size(victim), 7u);
	}
	// Landlock does not cover chmod: only the filter refuses it.
	if (!test_util::k_decoder_has_syscall_filter) {
		BOOST_TEST_MESSAGE("chmod skipped: a sanitizer build has no system call filter");
		return;
	}
	const auto refused = run_call(scratch, "required", "chmod", victim);
	BOOST_CHECK_MESSAGE(refused.exit_code == 0, "chmod: " << refused.exit_code << " " << refused.err);
	BOOST_CHECK((fs::status(victim).permissions() == before));
}

// io_uring, ptrace and a new process are refused by the filter alone (Landlock has no say in them).
BOOST_AUTO_TEST_CASE(ASandboxedChild_CannotUseIoUring_PtraceOrStartAProcess)
{
	if (!test_util::k_decoder_has_syscall_filter) {
		BOOST_TEST_MESSAGE("skipped: a sanitizer build has no system call filter");
		return;
	}
	const fs::path scratch = dir.path() / "scratch";
	for (const char* call : { "io_uring", "ptrace", "fork" }) {
		BOOST_TEST_CONTEXT(call) {
			const auto unsandboxed = run_call(scratch, "off", call);
			// Only io_uring may be refused by the system itself (sysctl, container profile): ptrace and fork must work.
			if (std::string_view(call) == "io_uring" && unsandboxed.exit_code == 0) {
				BOOST_TEST_MESSAGE("io_uring is refused without the sandbox too: not shown");
				continue;
			}
			BOOST_REQUIRE_MESSAGE(unsandboxed.exit_code == 5,
				call << ", off: " << unsandboxed.exit_code << " " << unsandboxed.err);
			const auto sandboxed = run_call(scratch, "best_effort", call);
			BOOST_CHECK_MESSAGE(sandboxed.exit_code == 0, call << ": " << sandboxed.exit_code << " " << sandboxed.err);
		}
	}
}
#endif

#if defined(_WIN32)
namespace {

struct confined_probe_result
{
	int exit_code = -1;
	std::string out;
	std::string err;
	std::string error;
};

// The test build of the decoder, run from a copy (a build folder is not readable by containers) in its AppContainer,
// policies and job, as process_decoder runs it (#1054).
confined_probe_result run_confined_probe(const fs::path& base, const fs::path& scratch, std::vector<std::string> arguments,
	bool confined = true, std::shared_ptr<const platform::app_container> container = platform::app_container::ensure(L"snk.model_decoder"))
{
	confined_probe_result result;
	std::string copy_error;
	const auto copy = edt::make_decoder_copy(SNK_MODEL_DECODER_SELFTEST_PATH, base, copy_error);
	if (!copy) {
		result.error = copy_error;
		return result;
	}
	fs::create_directories(scratch);
	platform::process_options options;
	options.working_dir = scratch;
	if (confined) options.confinement = platform::process_confinement{ std::move(container), { scratch }, { copy->folder } };
	options.on_stderr_line = [&result](std::string_view line) { result.err += std::string(line) + "\n"; };
	const auto run = platform::run_process(copy->program, arguments, options,
		[&result](std::string_view line) { result.out += std::string(line) + "\n"; }, {});
	result.exit_code = run.exit_code;
	result.error = run.error;
	return result;
}

}

BOOST_AUTO_TEST_CASE(AConfinedChild_IsInItsAppContainer_AndCannotOpenAFileOutsideItsFolder_ConnectOrStartAProgram)
{
	const fs::path base = dir.path() / "copies";
	const fs::path scratch = dir.path() / "scratch";
	const fs::path outside = dir.write("outside.txt", "a secret");

	const auto report = run_confined_probe(base, scratch, { "--sandbox", "required", "--self-test-sandbox-report" });
	BOOST_REQUIRE_MESSAGE(report.error.empty(), report.error);
	BOOST_CHECK_EQUAL(report.exit_code, 0);
	BOOST_CHECK_EQUAL(report.out, "fs=appcontainer syscalls=policy\n");

	const auto opened = run_confined_probe(base, scratch, { "--sandbox", "required", "--self-test-open", outside.string() });
	BOOST_CHECK_MESSAGE(opened.exit_code == 0, "open outside: " << opened.exit_code << " " << opened.err << opened.error);
	const auto linked = run_confined_probe(base, scratch, { "--sandbox", "required", "--self-test-hardlink", outside.string() });
	BOOST_CHECK_MESSAGE(linked.exit_code == 0, "hard link: " << linked.exit_code << " " << linked.err << linked.error);
	// A listener on the loopback: without the container the probe connects (5); in it, no capability, no connection.
	boost::asio::io_context io;
	boost::asio::ip::tcp::acceptor listener(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
	const std::string port = std::to_string(listener.local_endpoint().port());
	const auto connected = run_confined_probe(base, scratch, { "--sandbox", "required", "--self-test-connect-port", port });
	BOOST_CHECK_MESSAGE(connected.exit_code == 0, "connect: " << connected.exit_code << " " << connected.err << connected.error);
	const auto executed = run_confined_probe(base, scratch, { "--sandbox", "required", "--self-test-exec" });
	BOOST_CHECK_MESSAGE(executed.exit_code == 0, "exec: " << executed.exit_code << " " << executed.err << executed.error);

	// The control: the same probes outside the container succeed, or a 0 above proves nothing.
	const auto connected_off = run_confined_probe(base, scratch, { "--sandbox", "off", "--self-test-connect-port", port }, false);
	BOOST_CHECK_MESSAGE(connected_off.exit_code == 5, "connect off: " << connected_off.exit_code << " " << connected_off.err << connected_off.error);
	const auto executed_off = run_confined_probe(base, scratch, { "--sandbox", "off", "--self-test-exec" }, false);
	BOOST_CHECK_MESSAGE(executed_off.exit_code == 5, "exec off: " << executed_off.exit_code << " " << executed_off.err << executed_off.error);
	const auto opened_off = run_confined_probe(base, scratch, { "--sandbox", "off", "--self-test-open", outside.string() }, false);
	BOOST_CHECK_MESSAGE(opened_off.exit_code == 5, "open off: " << opened_off.exit_code << " " << opened_off.err << opened_off.error);
	const auto linked_off = run_confined_probe(base, dir.path() / "scratch_off", { "--sandbox", "off", "--self-test-hardlink",
		outside.string() }, false);
	BOOST_CHECK_MESSAGE(linked_off.exit_code == 5, "hard link off: " << linked_off.exit_code << " " << linked_off.err << linked_off.error);
}

// #1099: the folder of the copy is granted to the container once, however often the container starts from it.
BOOST_AUTO_TEST_CASE(TheCopysFolder_HoldsOneEntryForTheContainer_AfterManyStarts)
{
	const fs::path base = dir.path() / "copies";
	const auto container = platform::app_container::ensure(L"snk.model_decoder");
	BOOST_REQUIRE_MESSAGE(container->ok(), container->error());
	for (int run = 0; run < 3; ++run) {
		const auto report = run_confined_probe(base, dir.path() / ("scratch" + std::to_string(run)),
			{ "--sandbox", "required", "--self-test-sandbox-report" }, true, container);
		BOOST_REQUIRE_MESSAGE(report.exit_code == 0, report.exit_code << " " << report.err << report.error);
	}
	std::string copy_error;
	const auto copy = edt::make_decoder_copy(SNK_MODEL_DECODER_SELFTEST_PATH, base, copy_error);
	BOOST_REQUIRE_MESSAGE(copy.has_value(), copy_error);
	PACL acl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	BOOST_REQUIRE_EQUAL(GetNamedSecurityInfoW(copy->folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
		nullptr, &descriptor), static_cast<DWORD>(ERROR_SUCCESS));
	int entries = 0;
	for (DWORD index = 0; acl != nullptr && index < acl->AceCount; ++index) {
		void* entry = nullptr;
		if (!GetAce(acl, index, &entry)) continue;
		const auto* header = static_cast<const ACE_HEADER*>(entry);
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0) continue;
		const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
		if (EqualSid(const_cast<DWORD*>(&allowed->SidStart), container->sid())) ++entries;
	}
	LocalFree(descriptor);
	BOOST_CHECK_EQUAL(entries, 1);
}

// #1099: a child may empty its run folder and make the folder itself a junction (it may write and delete there). The
// editor then reads no answer through it, and removing the run folder removes the junction, not what it points at.
BOOST_AUTO_TEST_CASE(ARunFolderTheChildTurnedIntoAJunction_IsNeitherReadThroughNorEmptiedThrough)
{
	const fs::path kept = dir.write("outside/keep.txt", "the user's");
	dir.write("outside/out.bundle", "planted, not a bundle");
	std::string copy_error;
	const auto copy = edt::make_decoder_copy(SNK_MODEL_DECODER_SELFTEST_PATH, dir.path() / "copies", copy_error);
	BOOST_REQUIRE_MESSAGE(copy.has_value(), copy_error);
	auto cfg = selftest_config();
	cfg.decoder = copy->program;
	cfg.sandbox = edt::sandbox_mode::required;
	cfg.confine = true;
	cfg.container = platform::app_container::ensure(L"snk.model_decoder");
	cfg.confine_read_execute = { copy->folder };
	cfg.extra_arguments = { "--self-test-link-self", (dir.path() / "outside").string() };
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {}, {});
	BOOST_TEST_MESSAGE("the child: " << outcome.error);
	BOOST_CHECK(!outcome.bundle.has_value());
	BOOST_CHECK_MESSAGE(outcome.error.find("not a decoder bundle") == std::string::npos, outcome.error);
	BOOST_CHECK(fs::exists(kept));
	BOOST_CHECK(fs::exists(dir.path() / "outside" / "out.bundle"));
	BOOST_CHECK(scratch_is_empty());
}

BOOST_AUTO_TEST_CASE(AConfinementThatCannotBeSetUp_StartsNothing_AndSaysSo)
{
	platform::process_options options;
	options.confinement = platform::process_confinement{ platform::app_container::ensure(L""), {}, {} };
	const auto run = platform::run_process(SNK_MODEL_DECODER_SELFTEST_PATH, { "--self-test-sandbox-report" }, options,
		[](std::string_view) {}, {});
	BOOST_CHECK(run.confinement_failed);
	BOOST_CHECK_MESSAGE(run.error.find("AppContainer") != std::string::npos, run.error);
	BOOST_CHECK_EQUAL(run.exit_code, -1);
}
#endif

// #1054: a child may plant a link in its run folder that points at the user's files (a symlink; on Windows a junction).
// Removing the run folder after it removes the link, never what it points at. With the sandbox off, so the link is made.
BOOST_AUTO_TEST_CASE(ALinkTheChildPlantedInItsFolder_IsRemoved_NotFollowed)
{
	const fs::path kept = dir.write("outside/keep.txt", "the user's");
	auto cfg = selftest_config();
	cfg.sandbox = edt::sandbox_mode::off;
	cfg.extra_arguments = { "--self-test-link-out", (dir.path() / "outside").string() };
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {}, {});
#if defined(__linux__) || defined(_WIN32)
	BOOST_CHECK_MESSAGE(outcome.exit_code == 5, "the link was not planted: " << outcome.exit_code << " " << outcome.error);
#endif
	BOOST_CHECK(fs::exists(kept));
	BOOST_CHECK(scratch_is_empty());
}

// The editor learns how the child is sandboxed from its first stderr line, written before it read the request.
BOOST_AUTO_TEST_CASE(TheRunner_TakesTheSandboxStatus_FromTheChildsFirstLine)
{
	auto cfg = selftest_config();
	cfg.sandbox = edt::sandbox_mode::best_effort;
	cfg.extra_arguments = { "--self-test-pretend-no-landlock" };
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of(k_triangle_obj), {}, {});
	BOOST_REQUIRE_MESSAGE(outcome.ok(), outcome.error);
	BOOST_CHECK(outcome.sandbox.reported);
	BOOST_CHECK(!outcome.sandbox.fs);
	BOOST_CHECK_MESSAGE(!outcome.sandbox.why.empty(), outcome.sandbox.why);
}

BOOST_AUTO_TEST_CASE(AMissingDecoder_IsAFailure_NotACrash)
{
	auto cfg = config();
	cfg.decoder = dir.path() / "no_such_decoder";
	const edt::decode_runner runner(cfg);
	const auto outcome = runner.run(request_for("x.obj", edt::decode_request::mode_kind::scan), bytes_of("x"), {}, {});
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
