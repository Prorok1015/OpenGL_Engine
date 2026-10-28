#pragma once
#include <functional>
#include <snk/editor/edt_dialog_service.h>
#include <snk/project/prj_project.h>

namespace edt
{
	// Tells the user why the project given on the command line was not opened (EPIC-42 #156) and lets them go on
	// without it or quit. The answer goes into `project.answer`; a dialog closed without a button (Esc) counts as going
	// on, a dialog nobody answers (a headless run) leaves it unanswered, and the host then exits with an error code.
	// Opening the project from here (a relaunch with --allow-engine-version-mismatch) comes with #158.
	void ask_about_held_project(dialog_service& dialogs, prj::held_project& project, std::function<void()> quit);
}
