#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_migration_window_model.h"

namespace edt
{
	// Project Migration (EPIC-58 S8, #761): floating, closed by default; opened by Project > Migrate Project... (action
	// project.migrate.open, not for agents) and by --open-migration. It only draws migration_window_model: the plan (warnings
	// first, then the files with their steps, the project steps, what is not migrated, why it would be refused), Migrate, and
	// the report of the run. Text that comes from the project is drawn with TextUnformatted only.
	class migration_panel final : public panel_base
	{
	public:
		static constexpr const char* k_id = "project_migration";
		static constexpr const char* k_title = "Project Migration";

		migration_panel(editor_events& events, migration_window_model::ports ports);

		migration_window_model& model() { return m_model; }
		// Brings the window to the front on its next frame and asks for a fresh plan (unless a migration runs).
		void show_and_plan();

	protected:
		void on_before_begin() override;
		void on_render() override;

	private:
		void draw_idle();
		void draw_plan();
		void draw_running();
		void draw_report();
		void draw_error();

		migration_window_model m_model;
		bool m_focus = false;
	};
}
