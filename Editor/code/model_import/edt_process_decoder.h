#pragma once
#include "edt_async_import_task.h"
#include "edt_decode_bundle.h"
#include "edt_decode_codec.h"
#include "edt_decode_runner.h"
#include "edt_model_file_access.h"
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/resource/res_system.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace edt {

	// What the parent accepts of the decoder process's answer (#647). The child ran code on a model someone else made, so
	// its answer is checked against what was asked before anything of it is stored: these are pure, and the tests feed them
	// hostile bundles.

	// The answer to a scan: `kind` is "scan", no resources came with it, and every file it lists lies in the folder of the
	// model and within the limits of the request. A scan the child reported as failed comes back as a scan with its error.
	std::optional<model_file_scan> accept_scan_answer(const decode_bundle& bundle, const decode_request& request, std::string& problem);

	// The answer to an import: `kind` is "import", the resources that came with it are exactly its created tags (every one of
	// them there once), all tags are memory:// tags below the one folder of the prefab, and nothing of the model's path is
	// taken from the child (the request's is). An import the child reported as failed comes back with `error` set and no
	// resources.
	std::optional<import_intermediate> accept_import_answer(const decode_bundle& bundle, const decode_request& request, std::string& problem);

	// Whether `relative` (below the folder of an import named `stem`) is a name an import gives a resource: <stem>.prefab.desc,
	// <stem>.geom.desc, materials/*.mat.desc, textures/*.txm.desc and images, __embedded_txm/* descs and images. Never a model
	// file. The exporter checks the names it is about to write with it.
	bool is_import_resource_name(std::string_view relative, std::string_view stem);

	// The text after the last dot of the last segment, in lower case; empty when there is none.
	std::string file_extension_lowercase(std::string_view path);

	// The memory the decoder may use, from cfg `import/decoder_memory_mb`: 2 GiB when unset (0 or less), never unlimited, kept
	// between 256 MiB and 8 GiB; `warning` says so when the value was changed.
	std::uint64_t decoder_memory_limit(std::int64_t configured_mb, std::string& warning);

	// An agent's model parsed in the decoder process, with the same shape as the in-process importer: one function per
	// step of agent_import_options. Blocks (call it from a job). Anything that is not the model's own error (the decoder
	// was killed or failed, its answer was refused) is thrown as std::runtime_error with a text for the agent; the
	// resources of an import are stored in `resources` (memory://) only after the answer passed, and erased again when it
	// does not.
	class process_decoder
	{
	public:
		process_decoder(res::resource_system& resources, decode_runner_config config);

		// Empty when the decoder can be run; else why not, naming the file.
		std::string unavailable_reason() const;

		model_file_scan scan(const import_source& source, ds::task_context* task) const;
		import_intermediate import(const import_source& source, ds::task_context* task) const;

	private:
		decode_outcome run(const decode_request& request, const import_source& source, ds::task_context* task) const;

		res::resource_system& m_resources;
		decode_runner_config m_config;
	};

} // namespace edt
