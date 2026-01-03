#pragma once
#include "common.h"
#include "resources/res_resource_base.h"
#include "desc_system.h"
#include "resources/desc_resource.h"
#include "adapters/res_adapter_interface.hpp"

namespace scn
{
	// TODO: move to separate component with relation on assimp.
	class model_importer_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = res::adapter_info::make<desc::desc_resource>({ "glb", "obj", "fbx", "gltf" } );

		model_importer_adapter(desc::desc_system& desc_system_)
			: desc_system(desc_system_) {
		}

		std::shared_ptr<desc::desc_resource> operator()(const res::tag& tag, const std::vector<std::byte>& data) const;

		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}
		
		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override
		{
			ASSERT_FAIL("Model importer adapter does not support serialization!");
			return {};
		}

	private:
		desc::desc_system& desc_system;
	};
}