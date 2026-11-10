// snk_model_decoder: reads one model file into the engine's descs in a process of its own, so a parser that crashes,
// hangs or eats memory on a hostile file costs the editor nothing but a failed import (#647).
//
//   snk_model_decoder --request <bundle> --out <bundle>
//
// The request bundle holds the JSON of edt::decode_request and the model's bytes (the editor read and verified them).
// The out bundle holds a manifest ({"kind":"scan"|"import",...}) and, for an import, the resources it stored under
// memory://. Exit codes: 0 an out bundle was written (a refused model is an "error" inside it), 1 no out bundle could
// be made, 2 usage. Run it with the working directory a scratch folder: memory:// is ./sandbox there.
// The self-test flags (--self-test-*) exist only in the test build (SNK_MODEL_DECODER_SELF_TEST), never in the shipped tool.
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
#include <chrono>
#include <cstdlib>
#include <new>
#include <optional>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
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
void limit_address_space(std::uint64_t memory_limit_bytes)
{
#if defined(__linux__)
	if (memory_limit_bytes != 0 && k_address_space_limit_works) {
		const rlimit limit{ static_cast<rlim_t>(memory_limit_bytes), static_cast<rlim_t>(memory_limit_bytes) };
		setrlimit(RLIMIT_AS, &limit);
	}
#else
	(void)memory_limit_bytes;
#endif
}

struct arguments
{
	fs::path request;
	fs::path out;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	// For the process tests: sleep this long / allocate and touch this much / start a copy of this program that sleeps
	// and write its process id to a file, instead of decoding.
	std::uint64_t self_test_sleep_ms = 0;
	std::uint64_t self_test_alloc_bytes = 0;
	fs::path self_test_spawn_pid_file;
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
#if defined(SNK_MODEL_DECODER_SELF_TEST)
		} else if (arg == "--self-test-sleep" && has_value) {
			parsed.self_test_sleep_ms = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-alloc" && has_value) {
			parsed.self_test_alloc_bytes = std::strtoull(args[++i].string().c_str(), nullptr, 10);
		} else if (arg == "--self-test-spawn" && has_value) {
			parsed.self_test_spawn_pid_file = args[++i];
#endif
		} else {
			return std::nullopt;
		}
	}
#if defined(SNK_MODEL_DECODER_SELF_TEST)
	if (parsed.self_test_sleep_ms != 0 || parsed.self_test_alloc_bytes != 0 || !parsed.self_test_spawn_pid_file.empty()) {
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

boost::json::object failure_manifest(const char* kind, const std::string& error)
{
	boost::json::object manifest;
	manifest["kind"] = kind;
	manifest["error"] = error;
	return manifest;
}

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
#endif

	const auto request_bundle = edt::read_bundle(args->request);
	if (!request_bundle.ok()) {
		return fail("request: " + request_bundle.error);
	}
	const auto request = edt::decode_request_from_json(request_bundle.bundle->manifest);
	if (!request) {
		return fail("request: not a valid decode request");
	}
	limit_address_space(request->memory_limit_bytes);

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
	for (const edt::bundle_blob& blob : request_bundle.bundle->blobs) {
		if (blob.tag == edt::k_decode_source_tag) {
			source = &blob.bytes;
		}
	}
	if (!source) {
		return fail("request: the model bytes are missing");
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
	if (request->mode == edt::decode_request::mode_kind::import) {
		import.allowed = &request->allowed;
	}
	if (request->deadline_ms != 0) {
		import.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request->deadline_ms);
	}

	edt::decode_bundle out;
	try {
		if (request->mode == edt::decode_request::mode_kind::scan) {
			out.manifest["kind"] = "scan";
			out.manifest["scan"] = edt::to_json(edt::model_importer::scan_files(import));
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
