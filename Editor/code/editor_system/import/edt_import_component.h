#pragma once
#include "edt_async_import_task.h"
#include "edt_process_decoder.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ds { class task_system; }
namespace desc { class desc_system; }
namespace res { class resource_system; class tag; }

namespace edt
{
	class agent_model_import;
	class asset_export_dialog;
	class asset_exporter;
	class decoder_sandbox_notice;
	class editor_context;
	class editor_layer;
	class model_importer;
	class play_session;
	class process_decoder;

	// Bringing a model into the project (#548, #647, #925, #987): the decoder process and its OS sandbox notice, the adapter of the
	// models descs refer to, the person's import (file menu -> job -> export dialog), the asset exporter and the import an agent
	// asks for. Built without assimp (`SNK_HAS_ASSIMP=0`) it owns only the exporter, the dialog and the agent import, which then
	// answers that import is not available. `editor_system` creates it in init() and reads only the `blocker` questions below.
	class import_component
	{
	public:
		struct ports
		{
			editor_context& context;
			res::resource_system& resources;
			desc::desc_system& desc;
			ds::task_system& tasks;
			editor_layer& layer;
			// Play locks the document: no import starts while it runs. Null result: no Play session.
			std::function<const play_session*()> play = {};
			// The export dialog closed with an exported prefab: add it to the open scene (the caller checks that one is open).
			std::function<void(const res::tag&)> exported = {};
			// Tests only: the decoder steps every import runs; set, no decoder process is made (empty in the product).
			std::function<decoder_steps()> decoder = {};
		};

		explicit import_component(ports from);
		~import_component();
		import_component(const import_component&) = delete;
		import_component& operator=(const import_component&) = delete;

		// First in ~editor_system, before the jobs are cancelled: the adapter of models goes from the resource system (a load still
		// running keeps it, and with it the decoder, alive until it ends), the person's import job is cancelled and waited for,
		// and then the memory:// folders of imports are erased. Idempotent.
		void shutdown();

		agent_model_import* agent_import() { return m_agent_import.get(); }
		// The export dialog is open (an import waits in it).
		bool dialog_open() const { return m_dialog_open; }
		// The person's import runs (the job, or its result waiting for the main thread).
		bool person_import_running() const;
		bool agent_import_running() const;

#if SNK_HAS_ASSIMP
		// File > Import Model...: the job, the "Importing" popup (Esc cancels) and then the export dialog.
		void start_import(const std::filesystem::path& absolute_path);
#endif

	private:
		void build();
		bool poll_pipeline();
#if SNK_HAS_ASSIMP
		bool poll_progress();
		void finish_import();
		void erase_import_folder(std::string& folder);
		std::optional<std::string> agent_import_blocker() const;
#endif

		ports m_ports;
		bool m_shut_down = false;
		bool m_dialog_open = false;
#if SNK_HAS_ASSIMP
		std::unique_ptr<model_importer> m_importer;
		// Shared with the adapter of the models descs refer to (#925), which the resource system may outlive this component with.
		std::shared_ptr<process_decoder> m_decoder;
		// What every import runs: m_decoder's steps, or the ports' test steps.
		decoder_steps m_steps;
		// What the decoders of this run said of their OS sandbox, in Problems (#1076).
		std::shared_ptr<decoder_sandbox_notice> m_sandbox_notice;
		bool m_adapter_registered = false;
#endif
		std::unique_ptr<asset_exporter> m_exporter;
		std::unique_ptr<asset_export_dialog> m_export_dialog;
		// After the exporter it uses, before the context's job service goes.
		std::unique_ptr<agent_model_import> m_agent_import;
#if SNK_HAS_ASSIMP
		// The import in flight (m_importing); the job's on_done fills m_result or m_failure on the main thread.
		bool m_importing = false;
		bool m_finished = false;
		std::uint64_t m_job = 0;
		std::string m_name;
		float m_spinner_t = 0.f;
		std::optional<import_intermediate> m_result;
		// Set when the job itself failed (exception); a failed import reports through m_result->error.
		std::string m_failure;
		// The memory:// folders of the import in flight and of the one the export dialog shows (#987). The first goes with
		// finish_import whatever the outcome (a cancelled job never reports), the second when the dialog closes.
		std::string m_import_folder;
		std::string m_dialog_folder;
		std::uint64_t m_serial = 0;
		// Owner of the job callbacks; after the state they touch, so it dies (and cancels) first.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
#endif
	};
}
