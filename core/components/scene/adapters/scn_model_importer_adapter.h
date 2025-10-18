#pragma once
#include "common.h"
#include "res_resource_base.h"
#include "desc_system.h"
#include "desc_resource.hpp"

namespace scn
{
	// TODO: move to separate component with relation on assimp.
	class model_importer_adapter
	{
	public:
		static inline auto INFO = res::adapter_info::make<desc::desc_resource>({ "glb", "obj", "fbx", "gltf" } );

		model_importer_adapter(desc::desc_system& desc_system_)
			: desc_system(desc_system_) {
		}

		std::shared_ptr<res::Resource> operator()(const res::tag& tag, const std::vector<std::byte>& data) const;

	private:
		desc::desc_system& desc_system;
	};
}