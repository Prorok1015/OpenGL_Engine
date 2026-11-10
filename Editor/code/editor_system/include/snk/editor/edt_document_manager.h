#pragma once
#include "edt_api.h"
#include "edt_document.h"
#include "edt_registration.h"
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace edt
{
	class asset_kind_registry;
	class editor_events;
	class problem_list;
	struct asset_kind_desc;
	struct editor_services;

	// The documents of the editor: the one scene, and at most one open preview (the prefab, stage D). The scene is
	// owned by the editor, which builds its controllers on it before the editor_context exists; the manager gives it
	// its id and binds its document_changed to `events`. A preview is made by the asset kind that handles its source
	// (asset_kind_desc::make_preview, the longest matching suffix): the manager asks the kinds registry, keeps one
	// document per kind until that kind is removed, and closes and destroys it then. The editor reaches the concrete
	// scene and prefab documents through scene_of / prefab_of (controller/edt_document_cast.h).
	class SNK_EDITOR_API document_manager
	{
	public:
		document_manager(document& scene, editor_events& events);
		~document_manager();
		document_manager(const document_manager&) = delete;
		document_manager& operator=(const document_manager&) = delete;

		// Where previews come from: `kinds` (which must outlive the manager's use of it, or be destroyed first: the
		// subscription to its removals then ends with it), and the services a kind's make_preview gets. `problems`
		// (may be null) gets a warning when a preview with unsaved changes is closed because its kind was removed.
		// Not called: no preview can be opened.
		void set_preview_kinds(asset_kind_registry& kinds, editor_services& services, problem_list* problems = nullptr);

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

		// Opens the preview of `source`, closing the previous one first, and makes it active. Null when no asset kind
		// previews that source (nothing is closed). When the source cannot be loaded the returned document is not open
		// (is_open()) and nothing was announced.
		previewable_document* open_preview(const res::tag& source);
		// The open preview, null when there is none.
		previewable_document* preview();
		// Closes a preview; the scene stays open (the editor always edits one scene).
		void close(document_id id);

		// Visits the open documents: the scene first, then the preview.
		void for_each(const std::function<void(document&)>& fn);

	private:
		struct kind_preview
		{
			std::string kind_id;
			std::unique_ptr<previewable_document> instance;
		};

		previewable_document& preview_for(const asset_kind_desc& kind);
		void kind_removed(const asset_kind_desc& kind);

		document* m_scene;
		editor_events& m_events;
		asset_kind_registry* m_kinds = nullptr;
		editor_services* m_services = nullptr;
		problem_list* m_problems = nullptr;
		std::vector<kind_preview> m_previews;
		previewable_document* m_open_preview = nullptr;
		document_id m_active = 0;
		document_id m_next_id = 1;
		// Last: ends first, so no removal is reported to a half-destroyed manager.
		registration m_kind_removals;
	};
}
