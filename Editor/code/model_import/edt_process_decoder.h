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
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt {

	// What the parent accepts of the decoder process's answer (#647). The child ran code on a model someone else made, so
	// its answer is checked against what was asked before anything of it is stored: these are pure, and the tests feed them
	// hostile bundles.

	// The answer to a scan (#1043): `kind` is "scan", no resources came with it, and it asks for at most 256 files, each by
	// a name of 1..1024 bytes without a control character. Whether a name may be read is the editor's rule, applied
	// after this. A scan the child reported as failed comes back with its error.
	std::optional<scan_requests> accept_scan_answer(const decode_bundle& bundle, std::string& problem);

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

	// What the request says of `files` (decode_request::provided), and their bytes as `blobs`: one blob per file, found by
	// its shared bytes, however many names point at it. Throws std::runtime_error when the files together are bigger than
	// `max_total_bytes` or one file has more than k_max_names_per_file names.
	std::vector<provided_entry> provided_entries(const std::vector<provided_file>& files, std::uintmax_t max_total_bytes,
		std::vector<bundle_blob>& blobs);

	// The editor's side of a scan through the decoder (#1043): `run_round` runs the child once with the files handed over so far
	// and returns what it asks for (or throws). Every name is resolved, read and hashed by one model_file_access, so its
	// limits count over the whole scan; at most 3 rounds, all within `source.deadline` (the editor's reads included). A side
	// file the rule refuses is missing to the child (one WARN); any other refusal fails the scan.
	model_file_scan broker_scan(const import_source& source, const std::function<scan_requests(const std::vector<provided_file>&)>& run_round,
		ds::task_context* task);

	// The memory the decoder may use, from cfg `import/decoder_memory_mb`: 2 GiB when unset (0 or less), never unlimited, kept
	// between 256 MiB and 8 GiB; `warning` says so when the value was changed.
	std::uint64_t decoder_memory_limit(std::int64_t configured_mb, std::string& warning);

	// The decoder's sandbox mode from cfg `import/decoder_sandbox` (#1043): "best_effort" or "required"; anything else is
	// best_effort, and `warning` says so. An agent's import is `required` whatever this is (import_source::sandbox).
	sandbox_mode decoder_sandbox_mode(std::string_view configured, std::string& warning);

	// An agent's model parsed in the decoder process, with the same shape as the in-process importer: one function per
	// step of agent_import_options. Blocks (call it from a job). Anything that is not the model's own error (the decoder
	// was killed or failed, its answer was refused) is thrown as std::runtime_error with a text for the agent; the
	// resources of an import are stored in `resources` (memory://) only after the answer passed, and erased again when it
	// does not. The decoder opens no file of its own (#1043): the editor resolves, reads and hashes every side file by
	// model_file_access and hands the bytes over; scan asks in up to 3 rounds, import hands over exactly `source.allowed`.
	class process_decoder
	{
	public:
		process_decoder(res::resource_system& resources, decode_runner_config config);

		// Empty when the decoder can be run; else why not, naming the file.
		std::string unavailable_reason() const;
		// The SHA-256 of the decoder file, read once (#999); empty, with one WARN, when it cannot be read.
		std::string version() const;

		model_file_scan scan(const import_source& source, ds::task_context* task) const;
		import_intermediate import(const import_source& source, ds::task_context* task) const;

	private:
		decode_outcome run(const decode_request& request, const std::vector<bundle_blob>& files, const import_source& source,
			ds::task_context* task) const;

		res::resource_system& m_resources;
		decode_runner_config m_config;
		mutable std::once_flag m_version_once;
		mutable std::string m_version;
	};

	// The SHA-256 of `file`, hashed once per process for one path, size and write time (#1063): every process_decoder,
	// and each editor test makes one, asks for the decoder's. Empty when the file cannot be read.
	std::optional<std::string> file_version(const std::filesystem::path& file);
	// How many files file_version has hashed in this process.
	std::size_t file_version_hashes();

	// The steps of a decoder as the importers call them: process_decoder's, or a test's.
	struct decoder_steps
	{
		std::function<model_file_scan(const import_source&, ds::task_context*)> scan;
		std::function<import_intermediate(const import_source&, ds::task_context*)> import;
		// Empty when the decoder can run.
		std::function<std::string()> unavailable_reason;
		// What tells one build of the decoder from another. Unset, or empty: nothing it decoded is reused (#999).
		std::function<std::string()> version;
	};

	// The steps of `decoder`, which they keep alive (a load can outlive the editor_system that made it).
	decoder_steps steps_of(std::shared_ptr<const process_decoder> decoder);

	// Erases every file below memory://`folder` (ending in '/'): the decoder overwrites nothing, and memory:// is on the disk,
	// so an earlier run left its files there. The tag of a file that is still there afterwards, if any.
	std::optional<res::tag> erase_memory_folder(res::resource_system& resources, const std::string& folder);

} // namespace edt
