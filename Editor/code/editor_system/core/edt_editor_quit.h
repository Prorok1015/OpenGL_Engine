#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <string>

namespace edt
{
	// What editor.quit and the end of the stdio input act on (#452). All of it outlives the registration.
	struct editor_quit_sources
	{
		// The MCP session of the stdio transport; empty while there is none.
		std::function<std::string()> stdio_session;
		// Any open document has unsaved changes (any_unsaved).
		std::function<bool()> is_dirty;
		// Drops them in every open document.
		std::function<void()> discard_changes;
		// app_loop_service_interface::request_stop: the loop ends after the current frame, exit code 0.
		std::function<void()> request_stop;
	};

	// Visits the open documents (document_manager::for_each).
	using document_visitor = std::function<void(const std::function<void(document&)>&)>;
	// Any open document, not only the scene, with unsaved changes.
	bool any_unsaved(const document_visitor& for_each);

	// editor.quit exists only where nobody else can close the editor: an agent on the stdio pipes of a headless one.
	constexpr bool editor_quit_available(bool stdio_transport, bool headless) { return stdio_transport && headless; }

	// editor.quit{discard_unsaved?}: callable by the stdio session only (anyone else is `denied`); unsaved changes without
	// discard_unsaved are `needs_input`. Returns {stopping: true} and the loop stops at the end of the frame.
	[[nodiscard]] registration register_editor_quit(cmd::command_registry& commands, editor_quit_sources sources);

	// The stdio client closed stdin. Headless: nobody is left to drive the editor, so it drops unsaved changes (with a
	// warning) and stops. With a window the person keeps working: only logged. Main thread.
	void on_stdio_input_closed(bool headless, const editor_quit_sources& sources);
}
