#pragma once
#include "edt_document.h"
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>

namespace edt
{
	class editor_events;
	class scene_document;
	class prefab_document;
	struct editor_services;

	// The documents of the editor: the one scene, and at most one prefab preview (stage D). The scene is owned
	// by the editor, which builds its controllers on it before the editor_context exists; the manager gives it
	// its id, binds its document_changed to `events` and owns the preview. `services` (also the editor's) is what
	// the preview works through; it outlives the manager's use of it.
	class document_manager
	{
	public:
		document_manager(scene_document& scene, editor_services& services, editor_events& events);
		~document_manager();
		document_manager(const document_manager&) = delete;
		document_manager& operator=(const document_manager&) = delete;

		scene_document& scene();
		const scene_document& scene() const;
		// Closes the preview and lets go of the scene (document_changed stops); to be called before the editor
		// that owns the scene is destroyed. The manager only has its preview left afterwards.
		void shutdown();

		// The document edits and menus act on. Never null: the scene is always there.
		document* active();
		document* find(document_id id);
		// An id that names no open document is ignored.
		void set_active(document_id id);

		// Opens the preview of a prefab, closing the previous one first, and makes it active. When the prefab
		// cannot be loaded the returned document is not open (is_open()) and nothing was announced.
		prefab_document& open_prefab(const res::tag& prefab_tag);
		// The open preview, null when there is none.
		prefab_document* preview();
		// Closes a preview; the scene stays open (the editor always edits one scene).
		void close(document_id id);

		// Visits the open documents: the scene first, then the preview.
		void for_each(const std::function<void(document&)>& fn);

	private:
		scene_document* m_scene;
		editor_services& m_services;
		editor_events& m_events;
		std::unique_ptr<prefab_document> m_preview;
		document_id m_active = 0;
		document_id m_next_id = 1;
	};
}
