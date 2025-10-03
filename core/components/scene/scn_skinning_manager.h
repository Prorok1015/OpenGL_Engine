#pragma once
#include "common.h"
#include "entt/entt.hpp"
#include "rnd_driver_interface.h"
#include "rnd_ssbo_buffer_interface.h"
#include "desc_system.h"
#include "scn_skinning_prototype_desc.h"

namespace scn
{
	class skinning_manager
	{
	public:
		using skinned_ent = entt::entity;
		skinning_manager(desc::desc_system& sys)
			: descsys(sys) {}
		~skinning_manager() = default;

		rnd::driver::ssbo_buffer_interface* get_buffer(res::tag skin) {
			if (bone_indices_buffer.find(skin) != bone_indices_buffer.end()) {
				return bone_indices_buffer[skin].get();
			}
			return nullptr;
		}

		rnd::driver::ssbo_buffer_interface* get_weights_indeces_buffer(entt::handle ent, rnd::driver::driver_interface* driver);
	private:
		std::unique_ptr<rnd::driver::ssbo_buffer_interface> create_ssbo_weights_indeces_buffer(res::tag skin, rnd::driver::driver_interface* driver);

	private:
		std::unordered_map<res::tag, std::unique_ptr<rnd::driver::ssbo_buffer_interface>> bone_indices_buffer;
		desc::desc_system& descsys;
	};
}