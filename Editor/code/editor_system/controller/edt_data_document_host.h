#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace edt
{
	class data_document;
	class data_document_panel;
	class editor_context;
	struct editor_services;

	// The data documents that are open (EPIC-56, #793): each is owned by the document_manager (adopt) and shown in a
	// window of its own, which the panel registry holds through the registration here; the window goes first, it holds
	// the document. Opening what is open already moves the focus; closing one with unsaved changes asks first.
	class data_document_host
	{
	public:
		// `focus_other` takes a document that is open but not ours (the scene's level file, the prefab preview).
		data_document_host(editor_context& context, editor_services& services, std::function<desc_file_layout()> layout,
			std::function<void(document&)> focus_other);
		~data_document_host();
		data_document_host(const data_document_host&) = delete;
		data_document_host& operator=(const data_document_host&) = delete;

		// Opens `tag` as a data document, or focuses the document that has it. A file that is no data document is a
		// message window, or with `quiet` (a problem, a link: not a double click) a line in the log.
		void open(const res::tag& tag, bool quiet);
		// The window's close button: closes at once when clean, else asks to save, discard or keep.
		void request_close(document_id id);
		void close(document_id id);
		// The window's Save. A file that changed on disk is a question, not an error: reload it (the edits are lost),
		// overwrite it with this version, or cancel. Any other refusal is said in the window and the log. The save of the
		// close prompt asks the same, and Cancel keeps the window open.
		void request_save(document_id id);
		// The banner's buttons: Reload asks first when there are unsaved edits (they are lost), Overwrite always asks.
		void request_reload(document_id id);
		void request_overwrite(document_id id);
		bool owns(document_id id) const { return m_windows.contains(id); }

		std::vector<document_id> dirty() const;
		// Saves every dirty one; the first that cannot be saved is told to the person and stops it (false). A file that
		// changed on disk is asked about (Reload / Overwrite / Cancel) and stops it too: whatever is answered, the person
		// exits again (the answer comes later, the exit does not wait for it).
		bool save_all();
		// The windows and the subscription go before the documents do.
		void shutdown();

		// The one place that decides which document is active: the window that has the focus (ImGui's navigation window)
		// when it changed. A data window gives its document, the prefab editor window the preview's, a service window (the
		// dockspace host that holds the main menu bar, a popup, a tooltip, a modal, anything that takes no navigation focus,
		// a `##` name) or none changes nothing, any other window (viewport, hierarchy, inspector...) gives the scene. Windows
		// whose focus moved on record what they have not recorded yet. Called once per frame, with the frame's windows known.
		void follow_focus();

	private:
		struct window
		{
			std::shared_ptr<data_document_panel> panel;
			data_document* data = nullptr;
			registration registered;
		};

		void focus(document& open);
		void commit_pending_except(std::string_view focused_window);
		bool reload_now(document_id id);
		// Writes the document over a file that changed on disk; `done(true)` once it is written (after the version question
		// when it has one), `done(false)` after Cancel or a failure.
		void overwrite_now(document_id id, std::function<void(bool resolved)> done);
		// Reload / Overwrite / Cancel for a file that changed on disk; `done(true)` after a reload or an overwrite that
		// worked, `done(false)` after Cancel or a failure (empty `done`: nothing follows).
		void ask_conflict(document_id id, std::function<void(bool resolved)> done);
		bool in_conflict(document_id id) const;
		// A migrated file is written in its new version only after a yes (S56-6): `over` is the overwrite of a file that changed on
		// disk. `done(true)` after it was written, `done(false)` after Cancel or a refusal (empty `done`: nothing follows).
		void ask_version(document_id id, bool over, std::function<void(bool saved)> done);
		void show_error(document_id id, const std::string& text);

		// The root window that had the navigation focus at the last follow_focus (ImGuiID; 0: none).
		unsigned int m_focused_window = 0;
		editor_context& m_context;
		editor_services& m_services;
		std::function<desc_file_layout()> m_layout;
		std::function<void(document&)> m_focus_other;
		std::unordered_map<document_id, window> m_windows;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		connection m_closed;
	};
}
