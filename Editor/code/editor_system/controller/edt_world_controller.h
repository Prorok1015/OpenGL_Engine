#pragma once
#include <string>
#include <functional>
#include <entt/fwd.hpp>

namespace res { class resource_system; }
namespace desc { class desc_system; }

namespace edt
{
	struct shared_state;

	class world_controller
	{
	public:
		world_controller(shared_state& state, res::resource_system& res, desc::desc_system& desc);

		// False when it could not switch (no level, or the world is not in it); the state is left as it was then.
		bool switch_to_world(int idx);
		// switch_to_world / clear_active_world, and the panels hear about it even when the switch itself is refused
		// (the active index then stays on the old world, or becomes -1 when that is out of range):
		// the world arrays may have been replaced, so whatever points into them must be re-resolved.
		void select_world(int idx);
		// The level has no worlds: no world is active (active_world_idx = -1) and the panels drop their selection.
		void clear_active_world();
		void create_world(const std::string& name);

		// Callbacks set by the coordinator
		std::function<void(int idx)> on_world_switched;
		std::function<void(entt::registry&)> on_save_camera;
		std::function<void(entt::registry&)> on_inject_camera;

	private:
		shared_state& m_state;
		res::resource_system& m_res;
		desc::desc_system& m_desc;
	};
}
