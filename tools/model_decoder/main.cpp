// snk_model_decoder: reads one model file into the engine's descs in a process of its own, so a parser that crashes,
// hangs or eats memory on a hostile file costs the editor nothing but a failed import (#647).
//
//   snk_model_decoder --request <bundle> --out <bundle>
//
// The request bundle holds the JSON of edt::decode_request and the model's bytes (the editor read and verified them).
// The out bundle holds a manifest ({"kind":"scan"|"import",...}) and, for an import, the resources it stored under
// memory://. Exit codes: 0 an out bundle was written (a refused model is an "error" inside it), 1 no out bundle could
// be made, 2 usage. Run it with the working directory a scratch folder: memory:// is ./sandbox there.
#include "edt_decode_bundle.h"
#include "edt_decode_codec.h"
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log.h>
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
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
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

// The sanitizers reserve terabytes of address space for their shadow memory, so RLIMIT_AS cannot be used under them.
void limit_this_process(std::uint64_t memory_limit_bytes)
{
#if defined(__linux__)
	const pid_t parent = getppid();
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	// The parent may have died between our start and the prctl: the signal only comes for a death after it.
	if (getppid() != parent) {
		std::_Exit(1);
	}
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
	// For the process tests: sleep this long / allocate and touch this much instead of decoding.
	std::uint64_t self_test_sleep_ms = 0;
	std::uint64_t self_test_alloc_bytes = 0;
};

std::optional<arguments> parse_arguments(int argc, char** argv)
{
	arguments parsed;
	for (int i = 1; i < argc; ++i) {
		const std::string arg{ argv[i] };
		const bool has_value = i + 1 < argc;
		if (arg == "--request" && has_value) {
			parsed.request = fs::absolute(argv[++i]);
		} else if (arg == "--out" && has_value) {
			parsed.out = fs::absolute(argv[++i]);
		} else if (arg == "--self-test-sleep" && has_value) {
			parsed.self_test_sleep_ms = std::strtoull(argv[++i], nullptr, 10);
		} else if (arg == "--self-test-alloc" && has_value) {
			parsed.self_test_alloc_bytes = std::strtoull(argv[++i], nullptr, 10);
		} else {
			return std::nullopt;
		}
	}
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
	const auto args = parse_arguments(argc, argv);
	if (!args) {
		egLOG_ERROR("model_decoder", "usage: snk_model_decoder --request <bundle> --out <bundle>");
		return 2;
	}

	if (args->self_test_sleep_ms != 0) {
		limit_this_process(0);
		std::this_thread::sleep_for(std::chrono::milliseconds(args->self_test_sleep_ms));
		return 0;
	}

	const auto request_bundle = edt::read_bundle(args->request);
	if (!request_bundle.ok()) {
		return fail("request: " + request_bundle.error);
	}
	const auto request = edt::decode_request_from_json(request_bundle.bundle->manifest);
	if (!request) {
		return fail("request: not a valid decode request");
	}
	limit_this_process(request->memory_limit_bytes);

	if (args->self_test_alloc_bytes != 0) {
		std::vector<char> hog;
		try {
			hog.assign(static_cast<std::size_t>(args->self_test_alloc_bytes), 1);
		} catch (const std::bad_alloc&) {
			return 3;
		}
		return hog.empty() ? 1 : 0;
	}

	const std::vector<std::byte>* source = nullptr;
	for (const edt::bundle_blob& blob : request_bundle.bundle->blobs) {
		if (blob.tag == edt::k_decode_source_tag) {
			source = &blob.bytes;
		}
	}
	if (!source) {
		return fail("request: the model bytes are missing");
	}

	std::error_code ec;
	fs::create_directories("./res", ec);
	fs::create_directories("./sandbox", ec);

	// No threads: the import runs on this one.
	ds::task_system_config tasks_config;
	tasks_config.mode = ds::policy::manual;
	ds::task_system tasks(tasks_config);
	res::resource_system res_sys(tasks);
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
