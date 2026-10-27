#pragma once
#include <snk/common/common.h>
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_file_dialog.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <cstdint>
#include <memory>

namespace res {
	class resource_system;
}

namespace edt {

	class job_service;

	class asset_export_dialog {
	public:
		asset_export_dialog(asset_exporter& exporter, job_service& jobs, res::resource_system& resources);
		~asset_export_dialog();

		// Set the import result to export
		void open(import_result result);

		// Render the dialog. Returns false when dialog is closed.
		bool render();

		// After successful export, the res:// tag of the root prefab
		res::tag get_exported_tag() const { return m_exported_tag; }

	private:
		void render_export_form();
		void render_export_progress();

		asset_exporter& m_exporter;

		bool m_is_open = false;
		bool m_needs_open = false;
		import_result m_import;
		char m_asset_name[256] = {};
		char m_target_folder[512] = {};

		bool m_browse_folder = false;
		file_dialog m_folder_dialog;

		res::tag m_exported_tag;

		// Async export state
		job_service& m_jobs;
		res::resource_system& m_resources;
		std::uint64_t m_export_job = 0;
		std::shared_ptr<export_progress> m_export_progress;
		bool m_exporting = false;
		// Set by the job's on_done (main thread); render_export_progress closes the popup on it.
		bool m_export_done = false;
		// Last: destroyed first, so a job callback cannot run on a half-destroyed dialog.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};

} // namespace edt
