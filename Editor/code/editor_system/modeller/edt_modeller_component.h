#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/model_codec/res_tag.h>
#include <functional>
#include <memory>
#include <vector>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace inp { class input_router; }

namespace edt
{
	class editor_context;
	struct editor_services;
	class mesh_document;
	class mesh_editor_panel;

	// The mesh modeller, whole (EPIC-77, #1011): its window, the asset kinds and previews that make a mesh_document, the close question
	// and Save/Revert/Overwrite geometry of the shown mesh, and the Problems rows of the meshes whose geometry file is out of date.
	// editor_system creates it, calls install() and asks the few questions the file menu has about edited meshes.
	class modeller_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			rnd::render_system& rnd;
			gui::gui_system& gui;
			inp::input_router* input_router;
			// The project's desc files (late-bound: the asset opening is made after this component); without it a mesh cannot be saved.
			std::function<desc_file_layout()> desc_layout = {};
			// Play runs: no mesh can be edited or saved. Null: never.
			std::function<bool()> edits_locked = {};
		};

		explicit modeller_component(ports from);
		~modeller_component();

		// The window goes into the panel registry, the asset kinds (`.poly.desc`, `.geom.desc`) and their previews into the asset kind
		// registry, and the events the window follows are subscribed.
		void install(std::vector<registration>& out);

		// Opens the mesh of `source` (a poly or geometry file) in the window; the reason is shown when it cannot be opened.
		void open(const res::tag& source);
		// The window shows this document and comes to the front.
		void show(document_id id);
		// Closes the mesh after Save / Discard / Cancel when it has unsaved edits.
		void request_close(document_id id);
		// Exit, New and Open: any mesh with unsaved edits.
		bool dirty() const;
		// Saves every dirty mesh in the order opened; false (the reason shown) at the first one that was not saved.
		bool save_dirty();
		// Closes the meshes with unsaved edits without asking (Discard, quitting).
		void drop_dirty();
		// The game layer goes or the editor quits: the window lets go of its document first.
		void detach_document();

	private:
		mesh_document* shown() const;
		void show_in_window(mesh_document* mesh);
		void close_now(document_id id);
		void confirm_close(mesh_document& mesh, std::function<void()> proceed);
		void save_shown();
		void overwrite_geometry_of_shown();
		void revert_shown();
		void publish_problems();
		void report(const std::string& text);

		ports m_ports;
		std::shared_ptr<mesh_editor_panel> m_panel;
		std::shared_ptr<int> m_alive = std::make_shared<int>();
		document_id m_shown = document_id{};
		std::vector<connection> m_connections;
	};
}
