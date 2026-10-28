#include "edt_engine_version_prompt.h"
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

void edt::ask_about_held_project(dialog_service& dialogs, prj::held_project& project, std::function<void()> quit,
	std::function<void()> open_anyway)
{
	const std::string versions = std::format("The project '{}' was made for engine {}; this editor is engine {}.",
		project.name, prj::to_string(project.engine), prj::to_string(prj::current_engine_version()));
	const bool older = project.action == prj::version_action::ask_open_or_quit;
	const bool can_open = older && open_anyway;
	const std::string advice = !older
		? "It was not opened: open it with that editor version or a newer one."
		: can_open
		? "It was not opened. Open Anyway restarts the editor with it, without migrating anything (as "
		  "--allow-engine-version-mismatch does)."
		: "It was not opened. To open it without migrating anything, start the editor with --allow-engine-version-mismatch.";

	std::vector<std::string> options;
	if (can_open) options.push_back("Open Anyway");
	const std::size_t continue_index = options.size();
	options.push_back("Continue Without Project");
	const std::size_t quit_index = options.size();
	options.push_back("Quit");

	dialogs.choose("Engine Version", versions + "\n" + advice, std::move(options),
		[&project, quit = std::move(quit), open_anyway = std::move(open_anyway), continue_index, quit_index](
			std::optional<std::size_t> picked) {
			if (picked == quit_index) {
				project.answer = prj::held_project::decision::quit;
				quit();
				return;
			}
			project.answer = prj::held_project::decision::continue_without_project;
			if (picked && *picked < continue_index) {
				open_anyway();
			}
		});
}
