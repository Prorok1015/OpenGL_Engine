#pragma once
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>

namespace edt
{
	// True while something `prefab` needs to be built is still loading: a desc a component of any node refers to (a nested prefab is
	// looked into, each file once, down to 32 levels). A reference that failed to load is not waited for. No handle is waited on, nothing loads.
	bool has_pending_dependencies(const scn::prefab_desc& prefab);

	// The wait of the prefab editor for a prefab it is asked to open (#152, EPIC-25 US-25-7): the file is read and its references load in
	// the background, one poll a frame, so the window and the rest of the editor keep drawing; the preview is made when the poll says ready, from
	// what is cached by then. No ImGui: the component asks poll() from its frame callback.
	class prefab_open_watch
	{
	public:
		enum class state { idle, loading, ready, failed };

		// Starts (or replaces) the wait on the handle of the prefab's own load.
		void begin(res::res_handle<scn::prefab_desc> handle);
		void cancel();
		bool active() const { return m_active; }
		const res::tag& tag() const { return m_handle.get_tag(); }
		// loading: the file or something it refers to is not loaded yet; ready: all of it is; failed: the file did not load.
		state poll() const;

	private:
		res::res_handle<scn::prefab_desc> m_handle;
		bool m_active = false;
	};
}
