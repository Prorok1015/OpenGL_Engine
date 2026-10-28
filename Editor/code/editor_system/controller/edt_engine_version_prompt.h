#pragma once
#include <functional>
#include <snk/editor/edt_dialog_service.h>
#include <snk/project/prj_project.h>

namespace edt
{
	// Tells the user why the project given on the command line was not opened (EPIC-42 #156) and lets them go on
	// without it or quit. The answer goes into `project.answer`; a dialog closed without a button (Esc) counts as going
	// on, and a window closed with the dialog still open leaves it unanswered: the host then exits with an error code.
	// A non-interactive run (-n) never gets here: core_module refuses the project before the window opens (#403).
	// Opening the project from here (a relaunch with --allow-engine-version-mismatch) comes with #158.
	void ask_about_held_project(dialog_service& dialogs, prj::held_project& project, std::function<void()> quit);
}
