#pragma once
#include "edt_editor_camera.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/core/core_project_modules.h>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/scene/level/scn_input_stack_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_world_snapshot.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace inp { class input_router; }
namespace res { class resource_system; }

namespace edt
{
	class scene_document;
	class selection_service;
	class world_controller;
	struct editor_services;

	struct play_session_options
	{
		// Bindings and the mouse hold of the set "play" are dropped on Stop. Null: no router.
		inp::input_router* input = nullptr;
		// user:// of this resource system is served from `play_profile` while Play runs (emptied at every start) and
		// from the roots `editor_profile` again after Stop. Null or an empty path: user:// is left alone (no project).
		res::resource_system* profile_resources = nullptr;
		std::filesystem::path play_profile{};
		std::vector<std::filesystem::path> editor_profile{};
		// Shows the selection taken before Play again (the hierarchy, which goes on to select it); false: it could not.
		// Null: selection_service::select.
		std::function<bool(const selection&)> restore_selection{};
		// Why Play cannot start now besides the session's own reasons (a level or an asset still loading: the document
		// is about to change under it); nullopt: nothing. Null: nothing ever.
		std::function<std::optional<std::string>()> start_blocker{};
		// The raw entity ids the editor holds into Play's worlds (the Game view's pick, the gizmo's ring) go stale when
		// a module reload rebuilds them: called right after the set "play" is gone. Null: nothing to forget.
		std::function<void()> forget_runtime_entities{};
		// Runs `poll` on every UI frame under `id` while it returns true (editor_layer::register_implicit). Null: a
		// restored set runs at once, without waiting for its resources.
		std::function<void(const std::string& id, std::function<bool()> poll)> each_frame{};
		// The clock of that wait; null: std::chrono::steady_clock.
		std::function<std::chrono::steady_clock::time_point()> now{};
		// What the restored set waits for; null: the loads in flight of the game layer (resource_system::pending_loads).
		std::function<std::size_t()> pending_resources{};
		// Every module reload that met a running Play writes what it kept and dropped to
		// <project_root>/.cache/hot_reload/last_play_snapshot.json (HOT_RELOAD_PLAY.md 3.1). Empty: no report.
		std::filesystem::path project_root{};
		// The report also holds every world's whole state and the blackboard, not only their counts: costly in a big
		// world, it is written on the main thread.
		bool report_full_snapshot = false;
		// After every module reload that met Play: what Play lost, or why it restarted or stopped; empty when it kept all
		// of it. Null: the log only.
		std::function<void(const std::string& message)> notify{};
		// Play could not be restored in a build that loaded: asks for that build to be taken back (core::project_module_host
		// ::revert, never from inside the swap), whose swap brings Play back in the previous build; end_revert after the
		// call. Gets the module of that reload. Null: Play stops instead (HOT_RELOAD_PLAY.md 3.5).
		std::function<void(const std::string& module)> revert_module{};
	};

	// Play of the scene document (ADR-07, WORLD_CONTEXT.md decision 2): a session of the document, not a document. Start
	// builds the set "play" (game layer, no hot reload, contexts of kind play, mode Play, its own seed) from copies of
	// the document's worlds, never from files, hides the level (inactive, nothing extracted) and locks the document's
	// edits; Stop destroys the set and brings the editor back as it was before Start (level, editor camera, active
	// world, selection). Play itself changes neither the document nor the files; a Save during Play writes the document
	// as it was edited (the hidden level's transforms are those of before Start) and leaves its undo history alone.
	// Every change of state raises editor_events::play_state_changed. Main thread.
	class play_session
	{
	public:
		play_session(scene_document& document, editor_services& services, world_controller& worlds,
			selection_service& selection, editor_events& events, play_session_options options = {});
		// Stops a running session (reason user).
		~play_session();
		// The editor_context (selection, events) is already gone (the storage was torn down without editor_term), so
		// Stop cannot run: ~editor_system forgets the session without touching the editor.
		void abandon();
		play_session(const play_session&) = delete;
		play_session& operator=(const play_session&) = delete;

		play_state state() const { return m_state; }
		// Playing or paused: the set "play" exists.
		bool active() const { return m_state != play_state::stopped; }

		// Why start() would be refused now; nullopt: it would start.
		std::optional<std::string> start_refusal() const;
		// False (logged to "play") when refused: already active or stopping, no level with a world, a level or asset
		// load in flight (start_blocker), a leftover set "play".
		bool start(play_reason reason = play_reason::user);
		// Nothing when stopped or already stopping (a handler of an event raised during Stop may call it).
		void stop(play_reason reason = play_reason::user);
		// The set's mode Pause / Play. False when not active (or nothing to change).
		bool pause(play_reason reason = play_reason::user);
		bool resume(play_reason reason = play_reason::user);
		// `frames` (>= 1) frames of one fixed step each, then Pause (context_set::step). False when not active.
		bool step(std::uint32_t frames, play_reason reason = play_reason::user);
		// Pending frames of a step; 0 when not stepping or stopped.
		std::uint32_t pending_steps() const;
		// Frames the set "play" simulated since Start (context_set::frames_run); 0 when stopped.
		std::uint64_t frames_run() const;
		// Which Start this is (1 for the first of the editor's life): a mark that outlives a Stop, for anything that must
		// not be taken over by the next session (a runtime.query cursor).
		std::uint64_t run_number() const { return m_run_number; }
		// How many module reloads this Start's set was rebuilt by: entity ids taken before one name other entities.
		std::uint64_t reload_number() const { return m_reload_number; }
		// The worlds of the set "play" in its order; empty when stopped.
		std::vector<std::string> world_names() const;

		// Kept across sessions; applied at once to a running one.
		void set_time_scale(float scale);
		float time_scale() const { return m_time_scale; }
		void set_seed(std::uint64_t seed);
		std::uint64_t seed() const { return m_seed; }

		// Before a module reload (game_reloader_options::before_reload, HOT_RELOAD_PLAY.md 3.1): takes the snapshot that
		// keeps Play's world across the swap. Empty: kept, or Play is not running; else why Play restarts instead (a type
		// a snapshot cannot save).
		std::string capture_for_reload();
		// The game replaced the worlds of "play" with another level (scn::request_level, #564): the editor camera goes
		// into the new world that has the active world's name, the Game view and the viewport drop their picks
		// (forget_runtime_entities), the set's injected input and stack commands go. The document is not touched.
		void on_level_switched();
		// The reload call returned: a snapshot no swap used (the build was refused before the swap) is dropped.
		void end_reload()
		{
			if (m_suspended) return;
			m_reload.reset();
			m_capture_refusal.clear();
		}

		// The game module is swapped (module_swap_gate). With a snapshot the set "play" goes and the session stays
		// playing or paused, with nothing announced; after_module_swap rebuilds it from the snapshot in the new layer
		// (reloaded, rolled_back) or stops (stopped_for_reload) when that fails or the module is lost. Without one Play
		// stops (stopped_for_reload) and starts again (module_reloaded), paused if it was.
		void before_module_swap();
		void after_module_swap(const core::reload_report& report);
		// The revert revert_module asked for returned: one that never swapped (refused before its hooks) leaves Play
		// suspended, so it stops.
		void end_revert(const core::reload_report& report);
		// The revert revert_module asked for, on its frame: `revert` takes the build back (project_module_host::revert),
		// then end_revert. A Stop since the request cancels it instead: `forget` drops what could be taken back, the new
		// build stays loaded and Problems says so.
		void run_revert(const std::function<core::reload_report()>& revert, const std::function<void()>& forget);
		// The restore failed in the new build and its revert is asked for, not yet done: the build that loaded is taken
		// back. Stop drops the request.
		bool reverting() const { return m_reverting; }
		// Why the restore failed in the last reload; empty when it did not.
		const std::string& restore_failure() const { return m_restore_failure; }
		// Between before_module_swap and after_module_swap with a snapshot.
		bool suspended() const { return m_suspended; }
		// A module reload is under way: suspended, or the restored set waits (inactive, paused) for the resources its
		// components refer to (HOT_RELOAD_PLAY.md 6, step 6). Play's commands answer busy, the views show nothing;
		// pause()/resume() (the editor layout's) change the mode the set gets once the wait ends.
		bool reloading() const { return m_suspended || m_waiting.has_value(); }
		// What the restored set still waits for; 0 when it does not wait.
		std::size_t reloading_resources() const;
		// Longer than that, the set runs on with what has loaded (logged).
		static constexpr std::chrono::seconds k_resource_wait_limit{ 10 };

	private:
		void set_state(play_state state, play_reason reason);
		// The editor camera into the play twin of the active world, out of the twin it was in.
		void follow_active_world();
		void mount_user(const std::vector<std::filesystem::path>& roots, bool fresh);
		void restore_editor();
		void suspend_for_swap();
		void wait_for_resources();
		void end_wait(bool timed_out);
		std::chrono::steady_clock::time_point now() const;
		// False (with why): the set "play" may be left half built (stop() takes it). `restored` gets what the restore
		// dropped. May throw from the new module's code.
		bool restore_after_swap(scn::snapshot_report& restored, std::string& failure);
		// The half-built set goes; the session stays suspended with its snapshot, for the restore in the previous build.
		void await_revert(const std::string& module, std::string failure);
		// `play`: kept, reverted (kept in the previous build), stopped or restarted.
		void report_reload(const core::reload_report& outcome, const scn::snapshot_report& restored, std::string_view play);

		scene_document& m_document;
		editor_services& m_services;
		world_controller& m_worlds;
		selection_service& m_selection;
		editor_events& m_events;
		play_session_options m_options;

		play_state m_state = play_state::stopped;
		float m_time_scale = 1.0f;
		std::uint64_t m_seed;
		bool m_resume_after_swap = false;
		bool m_paused_before_swap = false;
		bool m_stopping = false;
		std::uint64_t m_run_number = 0;
		std::uint64_t m_reload_number = 0;

		// The copies the set "play" was built from; they live as long as it.
		std::vector<scn::world_desc> m_worlds_copy;
		// What a module reload keeps (HOT_RELOAD_PLAY.md 4): plain data, nothing of the module's binary.
		struct saved_world
		{
			std::string name;
			std::vector<std::string> systems;
			int input_layer = 0;
			std::optional<scn::input_stack_desc> input_stack{};
			boost::json::object state;
		};
		struct reload_snapshot
		{
			std::vector<saved_world> worlds;
			boost::json::array blackboard;
			// Written in the last frame, visible from the next one: put back as pending.
			boost::json::array blackboard_pending;
			ecs::run_mode mode = ecs::run_mode::play;
			std::uint64_t frames_run = 0;
			std::uint32_t pending_steps = 0;
			scn::snapshot_report report{};
			std::size_t bus_events = 0;
			// Systems of the module's layer that keep state in their fields: it starts over.
			std::vector<std::string> systems_restarted;
		};
		std::optional<reload_snapshot> m_reload;
		// Why the last capture_for_reload kept nothing: Play restarts instead.
		std::string m_capture_refusal;
		// "<world>: <system>" the last restore left out: the new build has no such system.
		std::vector<std::string> m_dropped_systems;
		// The restore in the new build failed and its revert was asked for (once per reload): why it failed.
		bool m_reverting = false;
		std::string m_restore_failure;
		// Why that revert did not bring the previous build back.
		std::string m_revert_failure;
		// When the reload began (capture_for_reload), for the report's duration.
		std::optional<std::chrono::steady_clock::time_point> m_reload_started;
		bool m_suspended = false;
		// How the restored set runs once its resources are there.
		struct resource_wait
		{
			ecs::run_mode mode = ecs::run_mode::play;
			std::uint64_t frames_run = 0;
			std::uint32_t pending_steps = 0;
			std::chrono::steady_clock::time_point since{};
		};
		std::optional<resource_wait> m_waiting;
		// Moves whenever a wait starts or ends, so the poll of an older one stops.
		std::uint64_t m_wait_generation = 0;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		// What Stop brings back.
		struct snapshot
		{
			editor_camera_state camera;
			std::string active_world;
			selection selected;
			bool level_active = true;
			std::vector<std::pair<std::string, bool>> extract;
		};
		snapshot m_before;
		// The editor camera as it moves in the play twins; the world whose twin holds it.
		editor_camera_state m_play_camera;
		std::string m_camera_world;
		connection m_world_changed;
	};
}
