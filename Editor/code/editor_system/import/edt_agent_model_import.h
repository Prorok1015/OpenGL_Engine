#pragma once
#include "edt_agent_source_rule.h"
#include "edt_asset_exporter.h"
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include "edt_model_import_limits.h"
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

	// What a request resolves to: the file the person is shown and approves, and the files the job will write. An
	// agent's plan made for the question holds the source as given and no size, time or identity: nothing on its disk
	// was looked at (text_plan). The start resolves it on the disk; the targets of the two must agree.
	struct agent_import_plan
	{
		std::filesystem::path source;	// on the disk: resolved, absolute, no link on the way
		bool on_disk = false;
		std::uintmax_t size = 0;
		std::filesystem::file_time_type modified{};
		file_id id{};
		// An agent's plan: the stamp of the handle its walk opened (#1412), in place of `modified` and `id`, which a
		// look by the path would take.
		std::optional<platform::beneath::file_stamp> stamp{};
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
		// What the decoder said of its OS sandbox when it listed the files (#1076): the second question says so.
		decoder_sandbox_status sandbox{};
		// Set when the request is refused: the command code (invalid_args, not_found, needs_input, busy) and why.
		std::string code;
		std::string error;

		bool ok() const { return error.empty(); }
		bool same_file_as(const agent_import_plan& other) const;
		// The same name, folder and prefab: what a person was shown of the files the import writes.
		bool same_targets(const agent_import_plan& other) const;
	};

	struct agent_import_options
	{
		// Reads the model and puts its resources into memory://. Null: the editor was built without assimp.
		std::function<import_intermediate(const import_source&, ds::task_context*)> import;
		// Lists the files besides the model it would read, or why an agent may not import it.
		std::function<model_file_scan(const import_source&, ds::task_context*)> scan;
		// Empty or null: the import can run. Else why it cannot now (the decoder process is not there): the command is
		// refused as disabled with this text.
		std::function<std::string()> unavailable_reason;
		// The folders res:// is read from, the top one last: a target that exists in any of them is not overwritten.
		std::function<std::vector<std::filesystem::path>()> resource_layers;
		// The folder an import writes into (the top layer of the project that is open now).
		std::function<std::filesystem::path()> resources_path;
		// Why no import may start now (Play, the dialog import is running); null or nullopt: it may.
		std::function<std::optional<std::string>()> blocker;
		// What the decoder's OS sandbox is as far as the editor knows before a run (#1076); null: nothing is known, the
		// question carries no mark.
		std::function<decoder_sandbox_status()> sandbox_status;
		// For the lifetime of what a person was shown; null: steady_clock.
		std::function<std::chrono::steady_clock::time_point()> now;
		// The largest model file an agent may import: the job reads it into memory and parses it.
		std::uintmax_t max_source_bytes = k_max_agent_source_bytes;
		// How long the job may parse a model before it fails.
		std::chrono::seconds parse_time_limit{ k_agent_parse_time_limit };
		// What the model may make the import read besides itself.
		model_file_limits file_limits;
		// Where an agent's source and side files may lie (agent_source_path_problem); tests fake a remote drive or
		// mount.
		std::function<unsigned(char letter)> drive_type = system_drive_type;
		std::function<std::optional<std::string>()> mount_table = system_mount_table;
		// Tests: the reads of an agent's files (source_volume_ports::read_below); null: read_below_root.
		std::function<platform::beneath::read_result(const platform::beneath::read_request&,
			const platform::beneath::read_limits&, std::vector<std::byte>&)> read_below;
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

		// The import is built in (the editor has assimp) and can run (its decoder is there).
		bool available() const { return unavailable_reason().empty(); }
		// Empty when an agent's import can run; else why not (no assimp, or the decoder process is missing).
		std::string unavailable_reason() const;
		// An import job of this service is running.
		bool running() const;

		// Resolves the request on the disk (a stat, the canonical path, the size, the file's identity and the folders of
		// every layer) without reading the model. `person`: no source rule; else agent_source_path_problem first, on the
		// path given and on the path resolved. For an agent's call it runs only once a person approved it (start).
		agent_import_plan resolve(const agent_import_request& request, bool person) const;
		// resolve() of an agent's source past its walk: the stamp of the handle, nothing looked at by the path.
		agent_import_plan resolve_for_agent(const agent_import_request& request, const std::filesystem::path& given,
			agent_import_plan& plan) const;

		// What the person is asked: the source, the destination, or why the call will be refused. For an agent's call
		// it never looks at the source on the disk (only the project's layers): the path as given, no size. A person's
		// call is described from the disk. With `files_token` (the second call, after a job found that the model refers
		// to other files) it lists every one of those files with its size and the total; for an agent each of them
		// passes the source rule first. Remembers the plan for the start that follows, bound to `ticket` (the id of the
		// question the registry is about to put, cmd::call::confirmation; empty for a call it allows).
		std::optional<std::string> describe(const agent_import_request& request, const cmd::origin& caller,
			std::string_view ticket = {});

		struct started
		{
			std::uint64_t job = 0;
			std::string code;	// empty when the job started
			std::string error;
			bool ok() const { return code.empty(); }
		};
		// Anyone but a person needs the plan describe remembered for exactly this call: the command registry runs a
		// call the policy asks about only with an approved ticket, and describes every allowed call first
		// (describe_before_allowed_invoke). Only then is the source looked at on the disk, after the source rule. The
		// job reads and parses the model; when it refers to other files the job ends with {needs_files_approval,
		// files_token, file_count} and writes nothing: the agent calls again with the token, a person is shown those
		// files, and that job imports exactly them. `ticket` (the call's confirmation) must be the one the plan was
		// described with: an approved call runs the plan of its own question.
		started start(const agent_import_request& request, const cmd::origin& caller, std::string_view ticket = {});

		// Per agent session: one session cannot push another one's questions or file lists out.
		static constexpr std::size_t k_max_shown = 32;
		static constexpr std::chrono::minutes k_shown_lifetime{ 5 };
		static constexpr std::string_view k_model_extensions[] = { ".glb", ".gltf", ".obj", ".fbx" };

	private:
		struct shown_plan
		{
			std::string key;
			std::string session;
			std::string ticket;
			agent_import_plan plan;
			std::chrono::steady_clock::time_point at;
		};

		std::chrono::steady_clock::time_point now() const;
		source_volume_ports volumes() const;
		// Everything of a plan but the source: the name, the folder, the prefab, whether it is free, the blockers.
		void plan_targets(const agent_import_request& request, const std::string& stem, agent_import_plan& plan) const;
		// An agent's plan for the question, from the text of the request only.
		agent_import_plan text_plan(const agent_import_request& request) const;
		std::string scan_key(const agent_import_request& request, const cmd::origin& caller) const;
		std::string call_key(const agent_import_request& request, const cmd::origin& caller) const;
		void remember(std::string key, std::string session, std::string ticket, agent_import_plan plan);
		// The plan remembered for `key` is used up either way; it comes back only when it was described for `ticket`.
		std::optional<agent_import_plan> take(const std::string& key, std::string_view ticket);

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
