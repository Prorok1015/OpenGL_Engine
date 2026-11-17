#pragma once
#include "mdl_api.h"
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <memory>
#include <vector>

namespace res {
	class resource_system;
}

namespace mdl
{
	// The resource adapter of the model files descs name (res://x.glb as a level's prefab or a __parent) in a game and the
	// tools: imports the model in this process, stores the prefab at memory://<folder><name>.desc and aliases the model's
	// tag to it. The editor registers its own adapter under the same INFO (the decoder process, #925).
	class SNK_MODEL_IMPORT_API model_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = res::adapter_info::make<desc::desc_base>({ "glb", "obj", "fbx", "gltf" });

		model_adapter(desc::desc_system& descs, res::resource_system& resources);

		std::shared_ptr<desc::desc_base> operator()(const res::tag& tag, const std::vector<std::byte>& data) const;

		std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;

	private:
		desc::desc_system& m_descs;
		res::resource_system& m_resources;
	};

	// Registers model_adapter under model_adapter::INFO; unregistrate_adapter(model_adapter::INFO) takes it out.
	SNK_MODEL_IMPORT_API void register_model_adapter(res::resource_system& resources, desc::desc_system& descs);

	// Turns on (off by default) remembering what an import wrote, per process: an import of bytes imported before
	// under the same tag then writes that again and does not run assimp. The unit tests turn it on, as each test imports
	// the same models again (#977); the editor and a game rely on the resource cache. Turning it off forgets it all.
	SNK_MODEL_IMPORT_API void remember_model_imports(bool on);
	// How many times model_adapter ran assimp in this process.
	SNK_MODEL_IMPORT_API std::size_t model_importer_runs();
}
