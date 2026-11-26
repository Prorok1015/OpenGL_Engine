#pragma once
#include <snk/common/common.h>
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_file_dialog.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace res {
	class resource_system;
}

namespace gui {
	struct theme_semantic_colors;
}

namespace edt {

	class job_service;

	class asset_export_dialog {
	public:
		// `colors` (editor_context::colors()) must outlive the dialog.
		asset_export_dialog(asset_exporter& exporter, job_service& jobs, res::resource_system& resources,
			const gui::theme_semantic_colors& colors);
		~asset_export_dialog();

		// Set the import result to export. `reimport` (#1194): the prefab it is exported over; its name and folder are
		// fixed, and the files the earlier import made that this one does not are listed after the export.
		void open(import_result result, res::tag reimport = {});

		// Whether a document holds unsaved edits of a file the export would overwrite: Export is refused. Null: none.
		void set_open_check(std::function<bool(const res::tag&)> open_with_unsaved_changes)
		{
			m_open_with_unsaved_changes = std::move(open_with_unsaved_changes);
		}
		const res::tag& reimport_target() const { return m_reimport; }
		const char* asset_name() const { return m_asset_name; }
		const char* target_folder() const { return m_target_folder; }
		// After a re-import: what the earlier import made and this one did not (still in the project).
		const std::vector<res::tag>& no_longer_made() const { return m_left_over; }

		// Render the dialog. Returns false when dialog is closed.
		bool render();

		// After successful export, the res:// tag of the root prefab
		res::tag get_exported_tag() const { return m_exported_tag; }

	private:
		void render_export_form();
		void render_reimport_report();
		void render_export_progress();

		asset_exporter& m_exporter;
		const gui::theme_semantic_colors& m_colors;

		bool m_is_open = false;
		bool m_needs_open = false;
		import_result m_import;
		char m_asset_name[256] = {};
		char m_target_folder[512] = {};

		bool m_browse_folder = false;
		file_dialog m_folder_dialog;

		res::tag m_exported_tag;
		res::tag m_reimport;
		std::vector<res::tag> m_left_over;
		bool m_show_report = false;
		std::function<bool(const res::tag&)> m_open_with_unsaved_changes;

		// Async export state
		job_service& m_jobs;
		res::resource_system& m_resources;
		std::uint64_t m_export_job = 0;
		std::shared_ptr<export_progress> m_export_progress;
		bool m_exporting = false;
		// Set by the job's on_done (main thread); render_export_progress closes the popup on it.
		bool m_export_done = false;
		// Why the last export failed: the form stays open and shows it.
		std::string m_export_error;
		// Last: destroyed first, so a job callback cannot run on a half-destroyed dialog.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};

} // namespace edt
