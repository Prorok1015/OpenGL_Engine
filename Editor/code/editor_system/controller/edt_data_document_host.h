#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace edt
{
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
		bool owns(document_id id) const { return m_windows.contains(id); }

		std::vector<document_id> dirty() const;
		// Saves every dirty one; the first that cannot be saved is told to the person and stops it (false).
		bool save_all();
		// The windows and the subscription go before the documents do.
		void shutdown();

	private:
		struct window
		{
			std::shared_ptr<data_document_panel> panel;
			registration registered;
		};

		void focus(document& open);

		editor_context& m_context;
		editor_services& m_services;
		std::function<desc_file_layout()> m_layout;
		std::function<void(document&)> m_focus_other;
		std::unordered_map<document_id, window> m_windows;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		connection m_closed;
	};
}
