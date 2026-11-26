#pragma once
#include <snk/common/common.h>
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_file_dialog.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
		// fixed, and the files the earlier import made that this one does not are listed after the export. A re-import
		// first checks (an io job) what each file it would overwrite holds against the prefab's record (`made`).
		void open(import_result result, res::tag reimport = {});

		// Why a document's unsaved edits keep the export from overwriting a file (empty: they do not): Export is
		// refused with that text. Null: none does.
		void set_open_check(std::function<std::string(const res::tag&)> open_with_unsaved_changes)
		{
			m_open_with_unsaved_changes = std::move(open_with_unsaved_changes);
		}
		const res::tag& reimport_target() const { return m_reimport; }
		const char* asset_name() const { return m_asset_name; }
		const char* target_folder() const { return m_target_folder; }
		// After a re-import: what the earlier import made and this one did not (still in the project).
		const std::vector<res::tag>& no_longer_made() const { return m_left_over; }
		// Why that list may be incomplete (an old record, an unreadable prefab); empty when it is not.
		const std::string& no_longer_made_note() const { return m_left_over_note; }

		// The re-import's check of the files it would overwrite (#1194): running, and what it found.
		bool checking() const { return m_scanning; }
		const std::optional<reimport_scan>& checked() const { return m_scan; }
		// A file that changed after the import, or may have, would be overwritten: Export waits for the tick.
		bool needs_overwrite_tick() const;
		bool overwrite_ticked() const { return m_overwrite_ticked; }
		void set_overwrite_ticked(bool ticked) { m_overwrite_ticked = ticked; }
		// Why Export is disabled now; empty when it may run.
		std::string export_blocker() const;
		// What Export does: false (nothing starts) while export_blocker() says why not.
		bool start_export();
		bool exporting() const { return m_exporting; }
		// Why the last export failed or stopped; the form shows it.
		const std::string& export_error() const { return m_export_error; }

		// What holds Export of a re-import back: the check runs, it could not read a file, or a file that holds (or may
		// hold) edits made after the import would be overwritten without `ticked`. Empty when nothing does.
		static std::string reimport_blocker(bool checking, const reimport_scan* scan, bool ticked);
		// A re-import that has begun writing runs to its end (`export_progress::writing`, set right before the first
		// write): a cancel would leave the asset half overwritten.
		static bool may_cancel_export(bool reimport, bool writing) { return !reimport || !writing; }
		// The veto a re-import's export job gets: it refuses every cancel (job.cancel too) once `progress` is writing.
		static std::function<std::optional<std::string>(bool)> cancel_veto(std::shared_ptr<export_progress> progress);

		// Render the dialog. Returns false when dialog is closed.
		bool render();

		// After successful export, the res:// tag of the root prefab
		res::tag get_exported_tag() const { return m_exported_tag; }

	private:
		// Why the name, the folder or the files refuse the export (`remap` filled when they get that far).
		std::string form_problem(std::unordered_map<res::tag, res::tag>& remap) const;
		void start_check();
		// The job ended without a word (cancelled): a re-import says what it left, or how it ended when its work ran to
		// its end all the same (a cancel taken just before it began writing).
		void export_stopped();
		// What the export's job came to: `job_error` when it threw, else `returned` from export_to_project.
		void finish_export(const std::string& job_error, bool returned);
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
		std::string m_left_over_note;
		bool m_scanning = false;
		std::uint64_t m_scan_job = 0;
		std::uint64_t m_scan_serial = 0;
		std::optional<reimport_scan> m_scan{};
		std::optional<made_files> m_made{};
		bool m_overwrite_ticked = false;
		bool m_show_report = false;
		std::function<std::string(const res::tag&)> m_open_with_unsaved_changes;

		// Async export state
		job_service& m_jobs;
		res::resource_system& m_resources;
		std::uint64_t m_export_job = 0;
		std::shared_ptr<export_progress> m_export_progress;
		// What the running export wrote; read once its job is no longer running.
		std::shared_ptr<std::vector<res::tag>> m_written;
		std::shared_ptr<no_longer_made_list> m_left_over_result;
		// Set by the job's work as its last step: 0 not ended, 1 export_to_project returned true, 2 false.
		std::shared_ptr<std::atomic<int>> m_job_returned;
		bool m_exporting = false;
		// Set by the job's on_done (main thread); render_export_progress closes the popup on it.
		bool m_export_done = false;
		// Why the last export failed: the form stays open and shows it.
		std::string m_export_error;
		// Last: destroyed first, so a job callback cannot run on a half-destroyed dialog.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};

} // namespace edt
