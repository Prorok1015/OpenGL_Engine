#pragma once
#include "edt_desc_file_commands.h"
#include "edt_inspector_target.h"
#include <snk/command/cmd_types.h>
#include "edt_prefab_open_watch.h"
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace inp { class input_router; }

namespace edt
{
	class desc_problem_publisher;
	class editor_context;
	struct editor_services;
	class inspector_panel;
	class prefab_document;
	class prefab_editor_panel;
	class scene_document;
	struct save_all_ask;

	// The prefab editor, whole: its window, its document's close question and Apply/Revert, and the Inspector's hand-over
	// between the level and one node of the prefab's working copy. editor_system creates it, registers the window and asks it
	// the few questions the other areas have about an edited prefab; nothing else of the editor knows how it works.
	class prefab_editor_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			scene_document& level;
			rnd::render_system& rnd;
			gui::gui_system& gui;
			inp::input_router* input_router;
			inspector_panel& inspector;
			desc_problem_publisher& desc_problems;
			// The project's desc files (late-bound: the asset opening is made after this component); without it no variant is created.
			std::function<desc_file_layout()> desc_layout = {};
			// Runs `poll` every UI frame under `id` until it returns false (editor_layer::register_implicit); null: a prefab opens at once,
			// as before (#152). Late-bound like the other ports that name what is made after this component.
			std::function<void(std::string_view id, std::function<bool()> poll)> register_frame = {};
			// True while the person is typing a value or dragging (an ImGui item is active): a prefab that finishes loading then does
			// not take the window (#1303). Null: never.
			std::function<bool()> person_is_editing = {};
		};

		explicit prefab_editor_component(ports from);
		~prefab_editor_component();

		// The window goes into the panel registry here, so its place in the View menu is where the caller says. Subscribes
		// to the selection after the selection wiring of the panels has: an empty selection blanks the Inspector there.
		void install(std::vector<registration>& out);

		// The action `prefab.create_variant {base, name?}` (#151): a new `<name>.prefab.desc` next to `base` that holds only its `__parent` and `__layer`, opened
		// in the prefab editor. Also what the asset browser's "Create Variant" runs. Registered by install().
		// Opens the prefab for editing next to the ones already open (#1185): an open one is brought to the front, a prefab still
		// loading is not asked for twice. Every prefab loads on its own, so opening a second does not cancel the first.
		void open(const res::tag& tag);
		// The prefab of that file is still loading after open() (crash recovery waits for it, #1187).
		bool opening(const res::tag& tag) const;
		// The open prefab document of that file, else null.
		prefab_document* open_document(const res::tag& tag) const { return prefab_for(tag); }
		// The first open prefab with unapplied edits, else null.
		prefab_document* dirty() const;
		// The open prefab of that file when it has unapplied edits, else null.
		prefab_document* dirty_for(const res::tag& tag) const;
		// Applies every prefab with unapplied edits, one after the other; false (with the reason shown) at the first one that was
		// not applied, the rest are left as they are.
		bool apply_dirty();
		// The question of the dirty prefab Save All (controller/edt_save_all.h) could not apply: a file that changed on disk
		// (Reload / Overwrite / Cancel) or a variant whose edits need its base dropped (Save without base / Cancel).
		// `resolved(true)` once it was reloaded or written, `resolved(false)` after Cancel, a failure or when that prefab is not open.
		void resolve_unsaved(const save_all_ask& ask, std::function<void(bool resolved)> resolved);
		// The window's close button: closes the prefab the window shows (see request_close(id)); the window then shows another open one.
		void request_close();
		// The document tab's close: closes at once when that prefab has no unapplied edits, else asks Apply / Discard / Cancel
		// (Cancel keeps it open). The other prefabs are not touched.
		void request_close(document_id id);
		// Closes every prefab document and the window without asking, the Inspector first: it holds a pointer into the working copy.
		void close();
		// Ends every wait for a prefab being opened and leaves the window as it was: prefabs that were open stay open.
		void cancel_open();
		// Ends the wait of the load `id` (the banner's Cancel); the other loads and the open prefabs are not touched.
		void cancel_open(std::uint64_t id);
		// Closes each prefab document with unapplied edits without asking (Discard of Exit/New/Open); the window shows another open one, if any.
		void close_dirty();
		// The game layer goes: the prefab editor closes, with a WARN when that loses edits.
		void close_for_layer_change();
		// The prefab the window shows, null when none (what the Inspector's shape section rebuilds into).
		prefab_document* shown_prefab() const { return shown(); }
		// The Inspector shows a node of a prefab.
		bool owns_inspector() const { return m_inspector_target.is_prefab(); }
		// The prefab Edit > Undo acts on instead of the level: the one the Inspector's node belongs to, kept after an undo that removed that node
		// until a level selection takes the Inspector back or the prefab closes (null: none).
		prefab_document* inspector_prefab() const { return m_history_prefab != 0 ? prefab_by_id(m_history_prefab) : nullptr; }
		// The window opens (a data window of the host asked for it).
		void show();
		// The document goes first in editor_system's destructor: the window must not look at it any more.
		void detach_document();

	private:
		ports m_ports;
		std::shared_ptr<prefab_editor_panel> m_panel;
		// Weak copies of it keep the callbacks of dialogs from touching this component after it is gone.
		std::shared_ptr<int> m_alive = std::make_shared<int>();
		inspector_target m_inspector_target;
		document_id m_history_prefab = 0;
		// A prefab being opened: its own wait (several may load at once).
		struct pending_open
		{
			std::uint64_t id = 0;
			prefab_open_watch watch;
			bool depth_warned = false;
		};
		std::vector<std::unique_ptr<pending_open>> m_pending;
		std::uint64_t m_next_pending_id = 1;
		// Set while finish_open opens a document that must not take the window: the activation listener leaves the window alone.
		bool m_keep_shown = false;
		bool m_polling = false;
		// The prefab document the window shows: the last one brought to the front.
		document_id m_shown{};
		// What each open prefab's load reported: the scope "prefab" of Problems holds the one of the prefab the window shows.
		std::map<document_id, std::vector<res::diagnostic>> m_diagnostics;
		// Last member: they go before what they reach.
		std::vector<connection> m_connections;

		// Waits for the prefab and what it refers to to load in the background (prefab_open_watch), then finish_open(): the window says
		// "Loading" meanwhile and the rest of the editor keeps drawing.
		void open_now(const res::tag& tag);
		// The prefab documents (opened with the built-in 'prefab' preview), in the order they were opened.
		std::vector<prefab_document*> prefabs() const;
		prefab_document* prefab_by_id(document_id id) const;
		prefab_document* prefab_for(const res::tag& tag) const;
		// The one the window shows, null when none.
		prefab_document* shown() const;
		// The window shows `prefab`: the Inspector lets go of the previous one first (its node pointers are into another working copy).
		void show_document(prefab_document& prefab);
		// Problems' scope "prefab" shows the diagnostics of the shown prefab, nothing when none is shown.
		void publish_diagnostics();
		// A shown prefab went: the window moves to the one opened last, or shows nothing.
		void leave_shown();
		bool loading() const { return !m_pending.empty(); }
		void show_loadings();
		void close_document(document_id id);
		// `loaded`: the handle the wait held (it keeps the prefab cached while the document opens); empty: read it now.
		void finish_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& loaded);
		// The shown prefab is being worked on (unapplied edits, a node picked in it, an item in use): a prefab that finished loading
		// in the meantime opens as a tab and leaves the window as it is.
		bool person_is_working_in_shown() const;
		// One poll of the wait: false while it is still loading; true when it ended (opened, failed or timed out) or is not running.
		bool settle_open();
		void report_failed_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& handle, bool timed_out);
		// Runs `proceed` at once, or after Save / Discard when `prefab` has unapplied edits (Cancel: never).
		void confirm_close(prefab_document* prefab, std::function<void()> proceed);
		bool apply(prefab_document& prefab);
		cmd::result create_variant(const cmd::call& call);
		void select_node(const std::vector<std::uint64_t>& path);
		// `blank_inspector` false: the caller sets the Inspector's node itself right after (a level selection).
		void release_inspector(bool blank_inspector);
	};
}
