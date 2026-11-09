#pragma once
#include "edt_api.h"
#include "edt_document.h"
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>

namespace edt
{
	class editor_events;

	// Makes the document the preview shows; called once, at the first open_preview.
	using preview_factory = std::function<std::unique_ptr<previewable_document>()>;

	// The documents of the editor: the one scene, and at most one preview (the prefab, stage D). The scene is owned
	// by the editor, which builds its controllers on it before the editor_context exists; the manager gives it
	// its id, binds its document_changed to `events` and owns the preview, which `make_preview` builds on the
	// first open_preview (the editor knows what a prefab document is; the manager does not). The editor reaches
	// the concrete scene and prefab documents through scene_of / prefab_of (controller/edt_document_cast.h).
	class SNK_EDITOR_API document_manager
	{
	public:
		document_manager(document& scene, preview_factory make_preview, editor_events& events);
		~document_manager();
		document_manager(const document_manager&) = delete;
		document_manager& operator=(const document_manager&) = delete;

		document& scene();
		const document& scene() const;
		// Closes the preview and lets go of the scene (document_changed stops); to be called before the editor
		// that owns the scene is destroyed. The manager only has its preview left afterwards.
		void shutdown();

		// The document edits and menus act on. Never null: the scene is always there.
		document* active();
		document* find(document_id id);
		// An id that names no open document is ignored.
		void set_active(document_id id);

		// Opens the preview of `source`, closing the previous one first, and makes it active. When the source
		// cannot be loaded the returned document is not open (is_open()) and nothing was announced.
		previewable_document& open_preview(const res::tag& source);
		// The open preview, null when there is none.
		previewable_document* preview();
		// Closes a preview; the scene stays open (the editor always edits one scene).
		void close(document_id id);

		// Visits the open documents: the scene first, then the preview.
		void for_each(const std::function<void(document&)>& fn);

	private:
		document* m_scene;
		preview_factory m_make_preview;
		editor_events& m_events;
		std::unique_ptr<previewable_document> m_preview;
		document_id m_active = 0;
		document_id m_next_id = 1;
	};
}
