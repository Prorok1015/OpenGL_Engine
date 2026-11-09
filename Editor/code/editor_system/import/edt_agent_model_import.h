#pragma once
#include "edt_asset_exporter.h"
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include "edt_model_importer.h"
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_system.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt {

	// What an agent asks of file.import_model (#548).
	struct agent_import_request
	{
		agent_import_request() = default;
		agent_import_request(std::string path_, std::string name_ = {}, std::string folder_ = {}, std::string files_token_ = {})
			: path(std::move(path_)), name(std::move(name_)), folder(std::move(folder_)), files_token(std::move(files_token_)) {}

		std::string path;	// an absolute path on disk, anywhere
		std::string name;	// the asset name; empty: the stem of the file
		std::string folder;	// the folder below res://; empty: assets/<name>
		// From the result of a first call whose model refers to other files: the second call, which shows a person
		// exactly those files, carries it. Empty: the first call.
		std::string files_token;
	};

	// What a request resolves to: the file the person is shown and approves, and the files the job will write. Resolved
	// once when the question is put and again when the job starts; the two must agree.
	struct agent_import_plan
	{
		std::filesystem::path source;	// resolved: absolute, no link on the way
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id;
		// What the job that listed the files read: the job that imports them must read the same bytes.
		content_digest digest{};
		bool hashed = false;
		std::string name;
		std::string folder;
		std::string prefab;	// res:// tag of the prefab the import produces
		// The files besides the model that the import may read: what the person was shown (the second question).
		std::vector<referenced_file> files;
		std::uintmax_t files_bytes = 0;
		bool scanned = false;
		// Set when the request is refused: the command code (invalid_args, not_found, needs_input, busy) and why.
		std::string code;
		std::string error;

		bool ok() const { return error.empty(); }
		bool same_file_as(const agent_import_plan& other) const;
	};

	struct agent_import_options
	{
		// Reads the model and puts its resources into memory://. Null: the editor was built without assimp.
		std::function<import_intermediate(const import_source&, ds::task_context*)> import;
		// Lists the files besides the model it would read, or why an agent may not import it.
		std::function<model_file_scan(const import_source&, ds::task_context*)> scan;
		// The folders res:// is read from, the top one last: a target that exists in any of them is not overwritten.
		std::function<std::vector<std::filesystem::path>()> resource_layers;
		// The folder an import writes into (the top layer of the project that is open now).
		std::function<std::filesystem::path()> resources_path;
		// Why no import may start now (Play, the dialog import is running); null or nullopt: it may.
		std::function<std::optional<std::string>()> blocker;
		// For the lifetime of what a person was shown; null: steady_clock.
		std::function<std::chrono::steady_clock::time_point()> now;
		// The largest model file an agent may import: the job reads it into memory and parses it.
		std::uintmax_t max_source_bytes = 64ull * 1024ull * 1024ull;
		// How long the job may parse a model before it fails.
		std::chrono::seconds parse_time_limit{ 60 };
		// What the model may make the import read besides itself.
		model_file_limits file_limits;
	};

	// The file lists jobs found and a person has not been shown yet; defined in the .cpp.
	struct agent_import_scans;

	// The import of a model an agent asks for: no dialog, one background job that imports, exports into the project and
	// cleans up after itself. The person approves exactly the file `describe` shows; the job imports exactly that file.
	class agent_model_import
	{
	public:
		agent_model_import(asset_exporter& exporter, job_service& jobs, res::resource_system& resources, agent_import_options options);
		~agent_model_import();
		agent_model_import(const agent_model_import&) = delete;
		agent_model_import& operator=(const agent_model_import&) = delete;

		// The import is built in (the editor has assimp).
		bool available() const { return static_cast<bool>(m_options.import) && static_cast<bool>(m_options.scan); }
		// An import job of this service is running.
		bool running() const;

		// Resolves the request without running anything. Public for the tests; describe and start use it.
		agent_import_plan resolve(const agent_import_request& request) const;

		// What the person is asked: the full resolved path, the size and the destination, or why the call is refused;
		// only the file system is asked (a stat), the model is not read. With `files_token` (the second call, after a job
		// found that the model refers to other files) it lists every one of those files with its size and the total.
		// Remembers the plan for the start that follows the answer.
		std::optional<std::string> describe(const agent_import_request& request, const cmd::origin& caller);

		struct started
		{
			std::uint64_t job = 0;
			std::string code;	// empty when the job started
			std::string error;
			bool ok() const { return code.empty(); }
		};
		// `confirmed`: a person approved this call (the call carries its ticket). Only a call whose plan was shown, still
		// is the same file and is allowed now starts the job. The job reads and parses the model; when it refers to other
		// files the job ends with {needs_files_approval, files_token, files[], total_bytes} and writes nothing: the agent
		// calls again with the token, a person is shown those files, and that job imports exactly them.
		started start(const agent_import_request& request, const cmd::origin& caller, bool confirmed);

		// Per agent session: one session cannot push another one's questions or file lists out.
		static constexpr std::size_t k_max_shown = 32;
		static constexpr std::chrono::minutes k_shown_lifetime{ 5 };
		static constexpr std::string_view k_model_extensions[] = { ".glb", ".gltf", ".obj", ".fbx" };

	private:
		struct shown_plan
		{
			std::string key;
			std::string session;
			agent_import_plan plan;
			std::chrono::steady_clock::time_point at;
		};

		std::chrono::steady_clock::time_point now() const;
		std::string scan_key(const agent_import_request& request, const cmd::origin& caller) const;
		std::string call_key(const agent_import_request& request, const cmd::origin& caller) const;
		void remember(std::string key, std::string session, agent_import_plan plan);
		std::optional<agent_import_plan> take(const std::string& key);

		asset_exporter& m_exporter;
		job_service& m_jobs;
		res::resource_system& m_resources;
		agent_import_options m_options;
		std::vector<shown_plan> m_shown;
		std::shared_ptr<agent_import_scans> m_scans;
		std::uint64_t m_job = 0;
		// Last: destroyed first, so a job callback cannot run on a half-destroyed service.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};

} // namespace edt
