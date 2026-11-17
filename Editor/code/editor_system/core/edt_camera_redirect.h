#pragma once
#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>
#include <entt/fwd.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/resource/res_tag.h>

namespace edt
{
	// True for a texture target assigned by camera_redirector (as opposed to the editor/prefab cameras').
	bool is_game_camera_target(const res::tag& target);

	// In the editor the 3D view is shown inside an ImGui panel, so a camera must not draw into the
	// window's backbuffer (it would be painted over by the UI). Every camera that targets the
	// backbuffer is given its own texture target instead.
	//
	// The cameras of one scope (a context set) that have the same viewport share one texture: the worlds of a
	// set are layers of one picture, and the render layers (camera_component::layer) draw them on top of each
	// other. A camera with a viewport of its own gets a target for that viewport.
	//
	// The redirector also owns the lifetime of those textures: a target whose camera is gone (entity
	// destroyed, world unloaded or reloaded) is released through `release` at the end of the pass.
	// Usage per frame: begin_frame(), redirect() for each registry, end_frame(release).
	class camera_redirector
	{
	public:
		using release_fn = std::function<void(const res::tag&)>;

		void begin_frame();
		// `scope` (the set name) keeps the target tags of different sets apart; the cameras of every registry
		// redirected with the same scope share their target. Returns how many cameras were redirected by this call.
		std::size_t redirect(entt::registry& reg, std::string_view scope);
		void end_frame(const release_fn& release);

		// Convenience for every context of every set: begin_frame, redirect each, end_frame.
		void update(scn::context_manager& contexts, const release_fn& release);

		[[nodiscard]] std::size_t tracked_count() const { return m_known.size(); }

	private:
		std::vector<res::tag> m_known; // targets assigned by this redirector and still alive
		std::vector<res::tag> m_live;  // targets seen in the current frame
	};
}
