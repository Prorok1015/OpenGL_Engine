#pragma once
#include "edt_api.h"
#include "edt_document.h"
#include "edt_registration.h"
#include <snk/resource/res_tag.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	class asset_kind_registry;
	class editor_events;
	class problem_list;
	struct asset_preview_desc;
	struct editor_services;

	// The documents of the editor: the one scene, and at most one open preview (the prefab, stage D). The scene is
	// owned by the editor, which builds its controllers on it before the editor_context exists; the manager gives it
	// its id and binds its document_changed to `events`. A preview is made by the asset preview that handles its source
	// (asset_preview_desc, the longest matching suffix): the manager asks the registry, keeps one document per preview
	// id, and closes and destroys it when that preview is removed. The editor reaches the concrete
	// scene and prefab documents through scene_of / prefab_of (controller/edt_document_cast.h).
	class SNK_EDITOR_API document_manager
	{
	public:
		document_manager(document& scene, editor_events& events);
		~document_manager();
		document_manager(const document_manager&) = delete;
		document_manager& operator=(const document_manager&) = delete;

		// Where previews come from: `kinds` (which must outlive the manager's use of it, or be destroyed first: the
		// subscription to its removals then ends with it), and the services a preview's make_preview gets. `problems`
		// (may be null) gets a warning when a preview with unsaved changes is closed because its registration was removed.
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

		// Opens the preview of `source`, closing the previous one first, and makes it active. Null when no asset preview
		// handles that source (nothing is closed). When the source cannot be loaded the returned document is not open
		// (is_open()) and nothing was announced. The first form takes the preview with the longest matching suffix, so a
		// module's more specific preview may be the one; a caller that needs a certain document names it by id (second
		// form; null when no preview has that id).
		previewable_document* open_preview(const res::tag& source);
		previewable_document* open_preview(std::string_view preview_id, const res::tag& source);
		// The open preview, null when there is none.
		previewable_document* preview();
		// The adopted documents are not a member: editor_context holds this class by value, so its size is part of the
		// module ABI (the services after it are read by a module's inline code); they are kept by the .cpp, per manager.
		// A document of the editor's own (a data document, EPIC-56): it gets its id, hears `document_changed` through
		// bind_events, is announced (document_opened) and becomes the active one. The manager keeps it until close().
		document& adopt(std::unique_ptr<document> adopted);
		// The open document whose source() is `source` (the scene's level file, the preview's asset, a data document's
		// file), null when there is none: opening a file that is open already moves the focus instead of making a second one.
		// Sources are compared by their meaning, not their spelling: `res://a/./b.desc`, `res://a//b.desc` and `res:\\a\\b.desc` are one file.
		document* find_by_source(const res::tag& source);

		// Closes a document (a preview, an adopted one) and destroys an adopted one after announcing it; the scene stays
		// open (the editor always edits one scene).
		void close(document_id id);

		// Visits the open documents: the scene first, then the preview, then the adopted ones in the order they were opened.
		// The list is taken before the first visit: a visitor may close the document it is given.
		void for_each(const std::function<void(document&)>& fn);

	private:
		struct kept_preview
		{
			std::string preview_id;
			std::unique_ptr<previewable_document> instance;
		};

		previewable_document& instance_of(const asset_preview_desc& preview);
		previewable_document* open_with(const asset_preview_desc& preview, const res::tag& source);
		void preview_removed(const asset_preview_desc& preview);

		document* m_scene;
		editor_events& m_events;
		asset_kind_registry* m_kinds = nullptr;
		editor_services* m_services = nullptr;
		problem_list* m_problems = nullptr;
		std::vector<kept_preview> m_previews;
		previewable_document* m_open_preview = nullptr;
		document_id m_active = 0;
		document_id m_next_id = 1;
		// Last: ends first, so no removal is reported to a half-destroyed manager.
		registration m_kind_removals;
	};
}
