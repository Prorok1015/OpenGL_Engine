#include "edt_engine_version_prompt.h"
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

void edt::ask_about_held_project(dialog_service& dialogs, prj::held_project& project, std::function<void()> quit)
{
	const std::string versions = std::format("The project '{}' was made for engine {}; this editor is engine {}.",
		project.name, prj::to_string(project.engine), prj::to_string(prj::current_engine_version()));
	const std::string advice = project.action == prj::version_action::ask_open_or_quit
		? "It was not opened. To open it without migrating anything, start the editor with --allow-engine-version-mismatch."
		: "It was not opened: open it with that editor version or a newer one.";

	dialogs.choose("Engine Version", versions + "\n" + advice, { "Continue Without Project", "Quit" },
		[&project, quit = std::move(quit)](std::optional<std::size_t> picked) {
			if (picked == std::size_t{ 1 }) {
				project.answer = prj::held_project::decision::quit;
				quit();
				return;
			}
			project.answer = prj::held_project::decision::continue_without_project;
		});
}
