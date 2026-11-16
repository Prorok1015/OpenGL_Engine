#pragma once
#include "edt_desc_problems.h"
#include "edt_slow_load_notice.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_start_level.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ds { class task_system; }
namespace res { class resource_system; }

namespace edt
{
	class editor_context;
	class editor_layer;
	class level_controller;
	struct editor_services;
	class scene_document;

	// Where the diagnostics of a load of the level lead, by that load (`copy` is its working copy, `file` its own file); also used
	// when a level is reloaded in place.
	desc_problem_publisher::locator level_locator(const res::res_handle<scn::level_desc>& handle, const res::tag& copy, const res::tag& file);

	// The load of a level, whole: the working copy it is read from, the "Loading Level" window and its Esc, level.cancel_load, the
	// notice of a slow load, and the settle that follows (the level waits for the loads it started before "level loaded" and the
	// exit of a headless run). The document takes the level's tags only when the load succeeds; a failed or cancelled load leaves
	// it as it was. editor_system creates it, calls install(), and hands begin_and_watch to everything that opens a level.
	class level_load_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_layer& layer;
			level_controller& level_ctrl;
			editor_services& services;
			res::resource_system& resources;
			scene_document& state;
			desc_problem_publisher& desc_problems;
			ds::task_system* tasks;
			// Another level replaces the one Play was built from: stops it when it runs. Play is created after this component.
			std::function<void()> stop_play;
			// The rows of the source "world" follow the level that was just loaded.
			std::function<void()> refresh_world_problems;
			// Set with editor/exit_after_level_loaded: stops the frame loop once the loaded level settled. Null: nothing.
			std::function<void()> stop_after_level_loaded;
		};

		explicit level_load_component(ports from);
		~level_load_component();
		level_load_component(const level_load_component&) = delete;
		level_load_component& operator=(const level_load_component&) = delete;

		// File > Cancel Level Load (level.cancel_load).
		void install(std::vector<registration>& out);

		// Starts loading `original_tag`; false when it is no level (nothing started).
		bool begin(const res::tag& original_tag);
		// True while the load goes on; when it ends, `loaded` (if given) says whether the level is loaded.
		bool poll(bool* loaded = nullptr);
		// begin(), then polls every UI frame under "edt/load_level" until the load ends; `ended(loaded)` runs once when it does.
		bool begin_and_watch(const res::tag& original_tag, std::function<void(bool)> ended = {});
		// A load that ended without a level, or was interrupted: the document keeps what it had, the working copy is removed.
		void abandon();
		// level.cancel_load, Esc in the loading window: the load goes, the level is the one before it.
		cmd::result cancel(const cmd::origin& from);

		bool in_progress() const { return m_loading; }
		// The level being loaded; empty when nothing loads.
		const res::tag& loading_tag() const { return m_tag; }
		// The level waits for the loads it started before it counts as loaded.
		bool settling() const { return m_settle.has_value(); }
		void cancel_settle();
		// The `editor_level_settle` frame stage.
		void tick_settle();

	private:
		void release();
		void erase_loading_copy(const res::tag& copy);
		void start_settle(const res::tag& level);

		ports m_ports;
		bool m_loading = false;
		res::res_handle<scn::level_desc> m_handle;
		slow_load_notice m_slow_load;
		// The level being loaded: its file and the working copy it is read from. The document takes both only when the load
		// succeeds (a failed or cancelled load leaves it as it was, and a save in between is refused). Every load has a working
		// copy of its own (`memory://editor/loading/<n>/<name>.desc`), so one that is abandoned never overwrites the copy of the
		// level being edited, whatever the two files are called.
		res::tag m_tag;
		res::tag m_editor_tag;
		// The root __version of the bytes the load read (0: unknown).
		int m_file_version = 0;
		std::uint32_t m_serial = 0;
		// Kept while the level waits: the settle holds a reference to it. Declared first, so it is destroyed after it.
		std::shared_ptr<scn::context_manager> m_settle_contexts;
		std::optional<scn::level_settle> m_settle;
	};
}
