#pragma once
#include "edt_api.h"
#include "edt_document.h"
#include "edt_registration.h"
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	// A window host (EPIC-82 5.4): shows one document of the chain at a time. 0 names none.
	using host_id = std::uint64_t;

	// Where a document was opened from. `asset`: from the asset browser or a menu, the file is the identity. `instance`: from
	// a place inside `parent` named by `anchor` (a node and a field); the identity is the pair (parent, anchor), never the file.
	enum class link_kind { asset, instance };

	struct open_context
	{
		link_kind kind = link_kind::asset;
		document_id parent = 0;
		std::string anchor;
	};

	class asset_kind_registry;
	class editor_events;
	class problem_list;
	struct asset_preview_desc;
	struct editor_services;

	// The documents of the editor: the one scene, and the open previews (the prefab; one at a time unless the preview
	// is `multiple`, #1185). The scene is
	// owned by the editor, which builds its controllers on it before the editor_context exists; the manager gives it
	// its id and binds its document_changed to `events`. A preview is made by the asset preview that handles its source
	// (asset_preview_desc, the longest matching suffix): the manager asks the registry, keeps one document per preview
	// id (a `multiple` preview: one per source opened, destroyed when closed), and closes and destroys them when that preview
	// is removed. The editor reaches the concrete
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
		// The open preview a caller that knows of one means: the active document when it is a preview, else the one opened last;
		// null when there is none.
		previewable_document* preview();
		// Every open preview, in the order they were opened.
		std::vector<previewable_document*> previews();
		// The adopted documents are not a member: editor_context holds this class by value, so its size is part of the
		// module ABI (the services after it are read by a module's inline code); they are kept by the .cpp, per manager.
		// A document of the editor's own (a data document, EPIC-56): it gets its id, hears `document_changed` through
		// bind_events and is announced (document_opened). It does not become the active one: whoever shows it in a window the person
		// works in says so (set_active), a quiet open leaves the active document alone. The manager keeps it until close().
		document& adopt(std::unique_ptr<document> adopted);
		// The open document whose source() is `source` (the scene's level file, the preview's asset, a data document's
		// file), null when there is none: opening a file that is open already moves the focus instead of making a second one.
		// Sources are compared by their meaning, not their spelling: `res://a/./b.desc`, `res://a//b.desc` and `res:\\a\\b.desc` are one file.
		document* find_by_source(const res::tag& source);

		// Closes a document (a preview, an adopted one) and destroys an adopted one after announcing it; the scene stays
		// open (the editor always edits one scene).
		void close(document_id id);

		// Links and window hosts (EPIC-82 5.4, #1402). A link is a document plus where it was opened from; the tables live in the .cpp, per
		// manager, so the class has no field for them. A document without a link is a chain of its own (the level, a prefab opened by name).
		//
		// Adopts `opened` as an `instance` (or `asset`) link of `context`; `context.parent` must be open for an instance. When the
		// pair (parent, anchor) is linked already, that link's id is returned and `opened` is destroyed unopened; 0 when the parent is
		// not open (nothing is adopted). The window is not made here: a host is (open_host).
		document_id open_linked(std::unique_ptr<document> opened, const open_context& context);
		// Records an `asset` link for a document that is open already (a preview, a data document); false for an id that names none.
		bool link_existing(document_id id, const open_context& context);
		// The documents from the root down to `id`, `id` last. A document with no link: just itself. Empty for an unknown id.
		std::vector<document_id> chain_of(document_id id);
		// The `instance` link opened from `parent` at `anchor`.
		std::optional<document_id> find_link(document_id parent, const std::string& anchor);
		// The context `id` was opened with; nullopt when it has none.
		std::optional<open_context> link_of(document_id id);
		// Every open link below `id`, the deepest first (the order they close in).
		std::vector<document_id> descendants_of(document_id id);
		// Links that no host shows, in the order they were linked.
		std::vector<document_id> unhosted_links();

		// A host showing `link` (a document that has no link record gets an `asset` one). A link another host shows keeps that host:
		// its id is returned and no host is made. 0 for a document that is not open.
		host_id open_host(document_id link);
		// The host stops showing anything and goes. The document and its unsaved changes stay open.
		void close_host(host_id host);
		std::optional<host_id> host_of(document_id link);
		std::optional<document_id> host_link(host_id host);
		std::vector<host_id> hosts();
		// Back and forward along the chain: `host` shows `link`. When another host shows it, nothing moves and that host's id is
		// returned (focus goes by identity). The link `host` showed stays open without a host. 0: no such host or link.
		host_id set_host_link(host_id host, document_id link);
		// Tear-off (`to_host` 0): a new host shows `link`; the host that showed it goes back to the link's parent when that has no host,
		// else it closes. Dock-back (`to_host` set): `to_host` shows `link` and the host that showed it closes. The id of the host that
		// shows `link` now, 0 when nothing was done.
		host_id move_link(document_id link, host_id to_host);
		// Moves with every change of the links and the hosts: the windows redraw on it.
		std::uint64_t links_revision() const;

		// Visits the open documents: the scene first, then the preview, then the adopted ones in the order they were opened.
		// The list is taken before the first visit: a visitor may close the document it is given.
		void for_each(const std::function<void(document&)>& fn);

	private:
		// The previews of one asset_preview_desc: its one document (single kind) or the documents opened with it (multiple).
		struct kept_preview
		{
			std::string preview_id;
			bool multiple = false;
			// A single kind keeps its one document for the life of the kind; a multiple kind's documents live while open.
			std::vector<std::unique_ptr<previewable_document>> instances;
		};

		kept_preview& kind_of(const asset_preview_desc& preview);
		kept_preview* kind_holding(const previewable_document* instance);
		previewable_document* preview_by_id(document_id id);
		previewable_document* open_with(const asset_preview_desc& preview, const res::tag& source);
		// Closes the document and announces it; destroys nothing (the caller does, after the announcement).
		void end_preview(previewable_document& closing);
		void preview_removed(const asset_preview_desc& preview);
		// The links below `id` close first; the link of a document that closed goes, and the host that showed it too.
		void close_descendants(document_id id);
		void document_closed_link(document_id id);
		void close_document(document_id id);

		document* m_scene;
		editor_events& m_events;
		asset_kind_registry* m_kinds = nullptr;
		editor_services* m_services = nullptr;
		problem_list* m_problems = nullptr;
		std::vector<kept_preview> m_previews;
		// The open previews in the order they were opened: the document state of the several previews (#1185) is a member, not a
		// table in the .cpp, so it is found where the manager is (its size is in the editor layout key: editor_layout_revision 2).
		std::vector<previewable_document*> m_open_previews;
		// The last document of a `multiple` kind that did not open (nothing announced it): kept until the next open, as the
		// pointer open_preview returned for it must stay valid.
		std::unique_ptr<previewable_document> m_failed_preview;
		document_id m_active = 0;
		document_id m_next_id = 1;
		// Last: ends first, so no removal is reported to a half-destroyed manager.
		registration m_kind_removals;
	};
}
