#pragma once
#include "edt_desc_file_commands.h"
#include "edt_inspector_target.h"
#include <snk/command/cmd_types.h>
#include "edt_prefab_open_watch.h"
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <functional>
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
		};

		explicit prefab_editor_component(ports from);
		~prefab_editor_component();

		// The window goes into the panel registry here, so its place in the View menu is where the caller says. Subscribes
		// to the selection after the selection wiring of the panels has: an empty selection blanks the Inspector there.
		void install(std::vector<registration>& out);

		// The action `prefab.create_variant {base, name?}` (#151): a new `<name>.prefab.desc` next to `base` that holds only its `__parent` and `__layer`, opened
		// in the prefab editor. Also what the asset browser's "Create Variant" runs. Registered by install().
		// Opens the prefab for editing; asks first when another one has unapplied edits.
		void open(const res::tag& tag);
		// The open prefab when it has unapplied edits, else null.
		prefab_document* dirty() const;
		// Applies the prefab with unapplied edits; false (with the reason shown) when it was not applied.
		bool apply_dirty();
		// Closes the window and the document without asking, the Inspector first: it holds a pointer into the working copy.
		void close();
		// Ends the wait for a prefab being opened and leaves the window as it was: a prefab that was open stays open.
		void cancel_open();
		// Closes the document with unapplied edits, window and Inspector left as they are (quitting).
		void drop_dirty_document();
		// The game layer goes: the prefab editor closes, with a WARN when that loses edits.
		void close_for_layer_change();
		// The Inspector shows a node of the prefab (Edit > Undo has nothing to undo there, #147).
		bool owns_inspector() const { return m_inspector_target.is_prefab(); }
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
		prefab_open_watch m_open_watch;
		bool m_polling = false;
		bool m_depth_warned = false;
		// The document whose diagnostics the scope "prefab" of Problems holds; closing it clears them.
		document_id m_desc_document{};
		// Last member: they go before what they reach.
		std::vector<connection> m_connections;

		// Waits for the prefab and what it refers to to load in the background (prefab_open_watch), then finish_open(): the window says
		// "Loading" meanwhile and the rest of the editor keeps drawing.
		void open_now(const res::tag& tag);
		// `loaded`: the handle the wait held (it keeps the prefab cached while the document opens); empty: read it now.
		void finish_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& loaded);
		// One poll of the wait: false while it is still loading; true when it ended (opened, failed or timed out) or is not running.
		bool settle_open();
		void report_failed_open(const res::tag& tag, const res::res_handle<scn::prefab_desc>& handle, bool timed_out);
		// Runs `proceed` at once, or after Save / Discard when the prefab has unapplied edits (Cancel: never).
		void confirm_close(std::function<void()> proceed);
		bool apply(prefab_document& prefab);
		cmd::result create_variant(const cmd::call& call);
		void select_node(const std::vector<std::uint64_t>& path);
		// `blank_inspector` false: the caller sets the Inspector's node itself right after (a level selection).
		void release_inspector(bool blank_inspector);
	};
}
