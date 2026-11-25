#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <functional>
#include <memory>
#include <vector>

namespace edt
{
	class editor_context;
	struct editor_services;
	class shader_editor_panel;
	class shader_source_document;

	// The GLSL editor, whole (#1202): its window, the asset kinds that open a shader source (`.glsl .vert .frag .geom .comp .tesc .tese
	// .shader`) in it, the previews that make a shader_source_document, the close question, Save / Revert of the shown file and what Exit,
	// New and Open ask (`dirty`, `save_dirty`, `drop_dirty`). Files of a layer below the project open read-only. editor_system creates it
	// and calls `install()`.
	class shader_editor_component
	{
	public:
		struct ports
		{
			editor_context& context;
			// The project's files (late-bound: the asset opening is made after this component); without it nothing can be saved.
			std::function<desc_file_layout()> desc_layout = {};
			// Play runs: no shader can be edited or saved. Null: never.
			std::function<bool()> edits_locked = {};
		};

		explicit shader_editor_component(ports from);
		~shader_editor_component();

		// The window goes into the panel registry, the asset kinds and their previews into the asset kind registry, and the events the
		// window follows are subscribed.
		void install(std::vector<registration>& out);

		// Opens the shader source `source` in the window, at `line` (1-based, 0: where it was). False, with the reason shown, when it
		// cannot be opened.
		bool open(const res::tag& source, int line = 0);
		// The window shows this document and comes to the front.
		void show(document_id id);
		// Closes the shader source after Save / Discard / Cancel when it has unsaved edits (typing not handed over included).
		void request_close(document_id id);
		// Exit, New and Open: any shader source with unsaved edits.
		bool dirty() const;
		// Saves every dirty shader source in the order opened; false (the reason shown) at the first one that was not saved.
		bool save_dirty();
		// Closes the shader sources with unsaved edits without asking (Discard, quitting).
		void drop_dirty();
		// The editor quits: the window lets go of its document first.
		void detach_document();
		// The shader source the window shows; none: no document.
		document_id shown_id() const { return m_shown; }

	private:
		shader_source_document* shown() const;
		void show_in_window(shader_source_document* shader);
		void close_now(document_id id);
		void confirm_close(shader_source_document& shader, std::function<void()> proceed);
		void save_shown();
		void revert_shown();
		void report(const std::string& text);

		ports m_ports;
		std::shared_ptr<shader_editor_panel> m_panel;
		std::shared_ptr<int> m_alive = std::make_shared<int>();
		document_id m_shown = document_id{};
		std::vector<connection> m_connections;
	};
}
