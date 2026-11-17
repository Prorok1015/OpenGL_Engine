#pragma once
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <chrono>
#include <set>
#include <string>

namespace edt
{
	// True while something `prefab` needs to be built is still loading: a desc a component of any node refers to (a nested prefab is
	// looked into, each file once, down to 32 levels). A reference that failed to load is not waited for. No handle is waited on, nothing loads.
	// `settled` (optional) keeps the files whose whole subtree was seen loaded, so the next call does not walk them again; `too_deep`
	// (optional) is set when a nested prefab lies beyond the 32 levels and is not looked into.
	bool has_pending_dependencies(const scn::prefab_desc& prefab, std::set<std::string>* settled = nullptr, bool* too_deep = nullptr);

	// The wait of the prefab editor for a prefab it is asked to open (#152, EPIC-25 US-25-7): the file is read and its references load in
	// the background, one poll a frame, so the window and the rest of the editor keep drawing; the preview is made when the poll says ready, from
	// what is cached by then. No ImGui: the component asks poll() from its frame callback.
	class prefab_open_watch
	{
	public:
		enum class state { idle, loading, ready, failed, timed_out };
		using clock = std::chrono::steady_clock;
		// A handle that is neither ready nor failed after this long ends the wait (`timed_out`): nothing is waited for forever.
		static constexpr std::chrono::seconds k_deadline{ 30 };

		// Starts (or replaces) the wait on the handle of the prefab's own load.
		void begin(res::res_handle<scn::prefab_desc> handle, clock::time_point now = clock::now());
		void cancel();
		// Ends the wait and hands out the handle of the prefab's own load: whoever holds it keeps the loaded prefab (and so the cache
		// entry the editor opens it from) alive, a cancelled wait drops it.
		res::res_handle<scn::prefab_desc> take();
		bool active() const { return m_active; }
		const res::tag& tag() const { return m_handle.get_tag(); }
		const res::res_handle<scn::prefab_desc>& handle() const { return m_handle; }
		// loading: the file or something it refers to is not loaded yet; ready: all of it is; failed: the file did not load (or came
		// back empty); timed_out: still loading at the deadline.
		state poll(clock::time_point now = clock::now());
		// A nested prefab beyond 32 levels was met by a poll: it is not waited for.
		bool nesting_limit_hit() const { return m_too_deep; }

	private:
		res::res_handle<scn::prefab_desc> m_handle;
		bool m_active = false;
		bool m_too_deep = false;
		clock::time_point m_started{};
		std::set<std::string> m_settled;
	};
}
