// snk_model_decoder: reads one model file into the engine's descs in a process of its own, so a parser that crashes,
// hangs or eats memory on a hostile file costs the editor nothing but a failed import (#647).
//
//   snk_model_decoder --request <bundle> --out <bundle>
//
// The request bundle holds the JSON of edt::decode_request, the model's bytes and the bytes of the side files the editor
// read for it by its rule (#1043): this process opens no file but its run folder. The out bundle holds a manifest
// ({"kind":"scan","requests"|"error"} or {"kind":"import",...}) and, for an import, the resources it stored under
// memory://. Exit codes: 0 an out bundle was written (a refused model is an "error" inside it), 1 no out bundle could
// be made, 2 usage, 4 it refused to run without its OS sandbox (#1043, `--sandbox off|best_effort|required`, one line on stderr
// says why). Run it with the working directory a scratch folder: memory:// is ./sandbox there, and the sandbox leaves it
// nothing else to touch.
// The self-test flags (--self-test-*) exist only in the test build (SNK_MODEL_DECODER_SELF_TEST), never in the shipped tool.
#include "decoder_sandbox.h"
#include "edt_decode_bundle.h"
#include "edt_decode_codec.h"
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/config/cfg_variables.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/resource/res_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
#if defined(SNK_MODEL_DECODER_SELF_TEST)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool k_address_space_limit_works = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool k_address_space_limit_works = false;
#else
constexpr bool k_address_space_limit_works = true;
#endif
#else
constexpr bool k_address_space_limit_works = true;
#endif

// First thing main does, before anything of the request is read: whatever the model makes the parser do, the process
// leaves no core dump (it would hold the model's bytes, outside the run folder), no crash dialog and does not outlive its
// parent. POSIX also takes away privilege gain.
void harden_process(long expected_parent)
{
#if defined(__linux__)
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	const rlimit no_core{ 0, 0 };
	setrlimit(RLIMIT_CORE, &no_core);
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	// The signal only comes for a death after the prctl: a parent that died before it is seen here, against the pid the
	// parent passed (without one, against nothing: getppid() of an orphan is 1 or a subreaper, not a pid to trust).
	if (expected_parent > 0 && getppid() != static_cast<pid_t>(expected_parent)) {
		std::_Exit(1);
	}
#elif defined(_WIN32)
	(void)expected_parent;
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#else
	(void)expected_parent;
#endif
}

// --parent-pid <n>, read before anything else (the full parse comes after the hardening).
long parent_pid_argument(int argc, char** argv)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::string_view(argv[i]) == "--parent-pid") {
			return std::strtol(argv[i + 1], nullptr, 10);
		}
	}
	return 0;
}

// The sanitizers reserve terabytes of address space for their shadow memory, so RLIMIT_AS cannot be used under them.
// Windows: the editor's Job object limits the memory.
// False when the limit was asked for and could not be set: the caller ends, a decoder without its limit must not run.
bool limit_address_space(std::uint64_t memory_limit_bytes)
{
#if defined(__linux__)
	if (memory_limit_bytes != 0 && k_address_space_limit_works) {
		const rlimit limit{ static_cast<rlim_t>(memory_limit_bytes), static_cast<rlim_t>(memory_limit_bytes) };
		return setrlimit(RLIMIT_AS, &limit) == 0;
	}
#else
	(void)memory_limit_bytes;
#endif
	return true;
}

struct arguments
{
	fs::path request;
	fs::path out;
	edt::sandbox_mode sandbox = edt::sandbox_mode::best_effort;
	std::uint64_t max_output_bytes = 1ull << 30;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	// For the process tests: sleep this long / allocate and touch this much / start a copy of this program that sleeps
	// and write its process id to a file, instead of decoding.
	std::uint64_t self_test_sleep_ms = 0;
	std::uint64_t self_test_alloc_bytes = 0;
	fs::path self_test_spawn_pid_file;
	// For the sandbox tests (#1043), once the sandbox is applied: open this file / connect to TCP / start a program /
	// print what the sandbox applied, instead of decoding. Exit 0: refused (or printed), 5: it worked.
	fs::path self_test_open;
	bool self_test_connect = false;
	bool self_test_exec = false;
	bool self_test_sandbox_report = false;
	// Signal the parent, or make the kernel signal it for this process's I/O (fcntl F_SETOWN, F_SETSIG, O_ASYNC,
	// F_NOTIFY).
	bool self_test_signal = false;
	bool self_test_pretend_no_landlock = false;
	// Start a second thread before the sandbox is applied: the sandbox must refuse to cover only one of them.
	bool self_test_extra_thread = false;
#endif
};

// The command line as paths: on Windows main's char** is the ANSI code page, which loses a path with other letters (a
// Cyrillic profile name), so the wide command line is read instead.
std::vector<fs::path> command_line(int argc, char** argv)
{
	std::vector<fs::path> out;
#if defined(_WIN32)
	(void)argc;
	(void)argv;
	int count = 0;
	if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
		for (int i = 0; i < count; ++i) out.emplace_back(wide[i]);
		LocalFree(wide);
	}
#else
	for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
	return out;
}

std::optional<arguments> parse_arguments(const std::vector<fs::path>& args)
{
	arguments parsed;
	for (std::size_t i = 1; i < args.size(); ++i) {
		const std::string arg = args[i].string();
		const bool has_value = i + 1 < args.size();
		if (arg == "--request" && has_value) {
			parsed.request = fs::absolute(args[++i]);
		} else if (arg == "--out" && has_value) {
			parsed.out = fs::absolute(args[++i]);
		} else if (arg == "--parent-pid" && has_value) {
			++i;
		} else if (arg == "--sandbox" && has_value) {
			const std::optional<edt::sandbox_mode> mode = edt::sandbox_mode_from_name(args[++i].string());
			if (!mode) return std::nullopt;
			parsed.sandbox = *mode;
		} else if (arg == "--max-output-bytes" && has_value) {
			parsed.max_output_bytes = std::strtoull(args[++i].string().c_str(), nullptr, 10);
			if (parsed.max_output_bytes == 0) return std::nullopt;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
		} else if (arg == "--self-test-sleep" && has_value) {
			parsed.self_test_sleep_ms = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-alloc" && has_value) {
			parsed.self_test_alloc_bytes = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-spawn" && has_value) {
			parsed.self_test_spawn_pid_file = args[++i];
		} else if (arg == "--self-test-open" && has_value) {
			parsed.self_test_open = args[++i];
		} else if (arg == "--self-test-connect") {
			parsed.self_test_connect = true;
		} else if (arg == "--self-test-exec") {
			parsed.self_test_exec = true;
		} else if (arg == "--self-test-sandbox-report") {
			parsed.self_test_sandbox_report = true;
		} else if (arg == "--self-test-signal") {
			parsed.self_test_signal = true;
		} else if (arg == "--self-test-pretend-no-landlock") {
			parsed.self_test_pretend_no_landlock = true;
		} else if (arg == "--self-test-extra-thread") {
			parsed.self_test_extra_thread = true;
#endif
		} else {
			return std::nullopt;
		}
	}
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (parsed.self_test_sleep_ms != 0 || parsed.self_test_alloc_bytes != 0 || !parsed.self_test_spawn_pid_file.empty()
		|| !parsed.self_test_open.empty() || parsed.self_test_connect || parsed.self_test_exec || parsed.self_test_sandbox_report
		|| parsed.self_test_signal || parsed.self_test_extra_thread) {
		return parsed;
	}
#endif
	if (parsed.request.empty() || parsed.out.empty()) {
		return std::nullopt;
	}
	return parsed;
}

int fail(const std::string& message)
{
	egLOG_ERROR("model_decoder", "{}", message);
	return 1;
}

// The blob index (provided_entry::blob) that a blob's tag names; SIZE_MAX when it names none.
std::size_t provided_index(const std::string& tag)
{
	const std::string_view prefix = edt::k_decode_file_tag_prefix;
	if (!tag.starts_with(prefix)) return SIZE_MAX;
	const char* begin = tag.data() + prefix.size();
	const char* end = tag.data() + tag.size();
	std::size_t index = 0;
	const auto [last, error] = std::from_chars(begin, end, index);
	return error == std::errc{} && last == end ? index : SIZE_MAX;
}

boost::json::object failure_manifest(const char* kind, const std::string& error)
{
	boost::json::object manifest;
	manifest["kind"] = kind;
	manifest["error"] = error;
	return manifest;
}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
// The self-test probes of the sandbox: 0 when the system refused what the probe tried, 5 when it worked, 6 when it failed
// in a way that says nothing about the sandbox.
int self_test_probe(const arguments& args, const decoder_sandbox::status& applied)
{
	if (args.self_test_sandbox_report) {
		std::fputs((decoder_sandbox::report_line(applied) + "\n").c_str(), stdout);
		std::fflush(stdout);
		return 0;
	}
#if defined(__linux__)
	const auto refused = [](int error) { return error == EACCES || error == EPERM ? 0 : 6; };
	if (!args.self_test_open.empty()) {
		const int fd = ::open(args.self_test_open.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) return refused(errno);
		::close(fd);
		return 5;
	}
	if (args.self_test_connect) {
		const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return refused(errno);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(9);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		const int connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
		const int error = errno;
		::close(fd);
		return connected != 0 && refused(error) == 0 ? 0 : 5;
	}
	if (args.self_test_signal) {
		const pid_t parent = ::getppid();
		const int directory = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		const int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
		if (directory < 0 || flags < 0) return 6;	// the probe needs these allowed to test anything
		bool any_worked = false;
		const auto tried = [&](int result) {
			if (result == 0) any_worked = true;
			else if (refused(errno) != 0) any_worked = true;	// an error other than a refusal: not shown refused
		};
		// The kernel's signals are aimed at this process and ignored: a run without the sandbox must not signal the test.
		std::signal(SIGIO, SIG_IGN);
		std::signal(SIGUSR1, SIG_IGN);
		tried(::kill(parent, 0));
		tried(::fcntl(STDOUT_FILENO, F_SETOWN, ::getpid()));
		tried(::fcntl(STDOUT_FILENO, F_SETSIG, SIGUSR1));
		tried(::fcntl(STDOUT_FILENO, F_SETFL, flags | O_ASYNC));
		tried(::fcntl(directory, F_NOTIFY, DN_MODIFY));
		::close(directory);
		return any_worked ? 5 : 0;
	}
	if (args.self_test_exec) {
		// A program that exits 5: an exec that worked is seen as such, not as a refusal.
		std::string shell = "/bin/sh";
		std::string flag = "-c";
		std::string command = "exit 5";
		char* const program_args[] = { shell.data(), flag.data(), command.data(), nullptr };
		char* const no_environment[] = { nullptr };
		::execve(shell.c_str(), program_args, no_environment);
		return refused(errno);
	}
#endif
	return 5;
}
#endif

} // namespace

int main(int argc, char** argv)
{
	harden_process(parent_pid_argument(argc, argv));
	const auto args = parse_arguments(command_line(argc, argv));
	if (!args) {
		egLOG_ERROR("model_decoder", "usage: snk_model_decoder --request <bundle> --out <bundle>");
		return 2;
	}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (!args->self_test_spawn_pid_file.empty()) {
		// A helper for the test that kills its parent: starts a sleeping copy of this program, writes its process id, then
		// sleeps. The copy must not outlive this process. It is told its parent like the runner tells the decoder: a sanitizer
		// build starts slowly enough for the parent to die before the copy sets its death signal, and only the pid check
		// in harden_process sees that.
		const auto child = platform::spawn_detached(platform::current_executable(),
			{ "--self-test-sleep", "120000", "--parent-pid", std::to_string(platform::current_process_id()) });
		std::ofstream(args->self_test_spawn_pid_file) << child.process_id;
		std::this_thread::sleep_for(std::chrono::minutes(2));
		return 0;
	}
	if (args->self_test_sleep_ms != 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(args->self_test_sleep_ms));
		return 0;
	}
	if (args->self_test_extra_thread) {
		std::thread([] { std::this_thread::sleep_for(std::chrono::minutes(2)); }).detach();
	}
#endif

	// The sandbox before anything of the request is read (#1043): whatever the model makes the parser do, it does inside it.
	decoder_sandbox::settings sandbox_settings;
	sandbox_settings.mode = args->sandbox;
	sandbox_settings.max_output_bytes = args->max_output_bytes;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	sandbox_settings.pretend_no_landlock = args->self_test_pretend_no_landlock;
#endif
	const decoder_sandbox::status sandbox = decoder_sandbox::apply(sandbox_settings);
	if (!sandbox.refusal.empty()) {
		const std::string line = std::string(edt::k_decoder_sandbox_refusal_prefix) + sandbox.refusal + "\n";
		std::fputs(line.c_str(), stderr);
		std::fflush(stderr);
		return edt::k_decoder_sandbox_exit_code;
	}
	std::fputs((decoder_sandbox::status_line(sandbox) + "\n").c_str(), stderr);
	std::fflush(stderr);
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (!args->self_test_open.empty() || args->self_test_connect || args->self_test_exec || args->self_test_sandbox_report
		|| args->self_test_signal || args->self_test_extra_thread) {
		return self_test_probe(*args, sandbox);
	}
#endif

	auto request_bundle = edt::read_bundle(args->request);
	if (!request_bundle.ok()) {
		return fail("request: " + request_bundle.error);
	}
	const auto request = edt::decode_request_from_json(request_bundle.bundle->manifest);
	if (!request) {
		return fail("request: not a valid decode request");
	}
	if (!limit_address_space(request->memory_limit_bytes)) {
		return fail("the memory limit could not be set");
	}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (args->self_test_alloc_bytes != 0) {
		std::vector<char> hog;
		try {
			hog.assign(static_cast<std::size_t>(args->self_test_alloc_bytes), 1);
		} catch (const std::bad_alloc&) {
			return 3;
		}
		return hog.empty() ? 1 : 0;
	}
#endif

	const std::vector<std::byte>* source = nullptr;
	std::map<std::size_t, std::vector<std::byte>*> file_bytes;
	for (edt::bundle_blob& blob : request_bundle.bundle->blobs) {
		if (blob.tag == edt::k_decode_source_tag) {
			source = &blob.bytes;
			continue;
		}
		const std::size_t index = provided_index(blob.tag);
		if (index > UINT32_MAX || blob.tag != edt::provided_blob_tag(index)) {
			return fail("request: it carries bytes that belong to no file of the request");
		}
		file_bytes[index] = &blob.bytes;
	}
	if (!source) {
		return fail("request: the model bytes are missing");
	}
	// Every name of one file shares its one blob: the bytes are held once, however many names the model used.
	std::map<std::size_t, std::shared_ptr<const std::vector<std::byte>>> shared_bytes;
	std::vector<edt::provided_file> files;
	files.reserve(request->provided.size());
	for (const edt::provided_entry& entry : request->provided) {
		std::shared_ptr<const std::vector<std::byte>> bytes;
		if (entry.present) {
			const auto found = file_bytes.find(entry.blob);
			if (found == file_bytes.end()) {
				return fail("request: a file of the request names bytes it does not carry");
			}
			std::shared_ptr<const std::vector<std::byte>>& shared = shared_bytes[entry.blob];
			if (!shared) shared = std::make_shared<const std::vector<std::byte>>(std::move(*found->second));
			bytes = shared;
		}
		files.push_back({ entry.name, entry.relative, entry.kind, std::move(bytes) });
	}
	if (shared_bytes.size() != file_bytes.size()) {
		return fail("request: it carries bytes that belong to no file of the request");
	}

	// No watcher threads in the child.
	cfg::set_variable("res/watch_files", "false");
	std::error_code ec;
	fs::create_directories("./res", ec);
	fs::create_directories("./sandbox", ec);

	// No threads: the import runs on this one.
	ds::task_system_config tasks_config;
	tasks_config.mode = ds::policy::manual;
	ds::task_system tasks(tasks_config);
	// res:// is the empty ./res of this run folder, read-only: the decoder reads no project and writes nothing there.
	// memory:// is ./sandbox, where the import stores what it makes. Nothing else is mounted (no user://, engine://, cache://).
	res::mount_table mounts;
	mounts.push_back(res::mount{ .protocol = res::tag::default_protocol(), .roots = { fs::absolute("./res") },
		.access = res::mount_access::read_only, .writable = res::write_policy::allow_all(), .watch = res::watch_options{},
		.file_operations = false });
	mounts.push_back(res::mount{ .protocol = res::tag::memory, .roots = { fs::absolute("./sandbox") },
		.access = res::mount_access::read_write, .writable = res::write_policy::allow_all(), .watch = res::watch_options{},
		.file_operations = false });
	res::resource_system res_sys(tasks, std::move(mounts));
	desc::desc_system desc_sys(res_sys);
	res_sys.registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(res_sys), std::ref(desc_sys));
	scn::ecs_assembler assembler(desc_sys, res_sys);
	rnd::skinning_manager skinning;
	auto scene_types = scn::register_scene_types(desc_sys, assembler, skinning);
	engine::render::register_render_descs(desc_sys);

	edt::import_source import;
	import.path = request->source_path;
	import.preloaded = source;
	import.limits = request->limits;
	import.boundary = request->boundary;
	import.memory_folder = request->memory_folder;
	import.on_disk = request->on_disk;
	import.provided = &files;
	if (request->deadline_ms != 0) {
		import.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request->deadline_ms);
	}

	edt::decode_bundle out;
	try {
		if (request->mode == edt::decode_request::mode_kind::scan) {
			out.manifest = edt::to_json(edt::model_importer::list_requests(import));
			out.manifest["kind"] = "scan";
		} else {
			edt::model_importer importer(desc_sys, res_sys);
			const edt::import_intermediate imported = importer.import_background(import);
			out.manifest["kind"] = "import";
			out.manifest["import"] = edt::to_json(imported);
			if (imported.error.empty()) {
				for (const res::tag& tag : imported.created_tags) {
					const auto block = res_sys.fetch_data(tag);
					if (!block) {
						return fail("a stored resource cannot be read back: " + tag.string());
					}
					const auto& bytes = block->get();
					out.blobs.push_back({ tag.string(), std::vector<std::byte>(bytes.begin(), bytes.end()) });
				}
			}
		}
	} catch (const std::exception& e) {
		out = {};
		out.manifest = failure_manifest(request->mode == edt::decode_request::mode_kind::scan ? "scan" : "import", e.what());
	}

	std::string error;
	if (!edt::write_bundle(args->out, out, error)) {
		return fail("out: " + error);
	}
	return 0;
}
