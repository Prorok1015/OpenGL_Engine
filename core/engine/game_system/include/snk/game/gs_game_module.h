#pragma once
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_layer.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/common/snk_api.h>
#include <entt/locator/locator.hpp>
#include <entt/meta/context.hpp>
#include <memory>
#include <string_view>
#include <vector>

namespace scn { class context_manager; }

namespace gs
{
	// Base of a game module (MODULE_LAYER.md): it is layered(), so the module loader gives it a layer, and it fills
	// the layer in register_content. The layer goes away as a whole after shutdown, so nothing registered there needs
	// to be removed by hand. Header-only: in a module binary its code runs there, which is what attaches the binary's
	// own entt::locator<meta_ctx> to the layer's meta context.
	class game_module : public app::modules::module_interface
	{
	public:
		bool layered() const final { return true; }
		std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }

		void register_services(ds::app_data_storage& data) final
		{
			auto& layer = data.require<scn::scene_layer>();
			// In a static build this binary's locator is the engine's, which must stay on the engine's context.
			const entt::meta_ctx* engine = &ecs::meta_context();
			m_attached = !entt::locator<entt::meta_ctx>::has_value() || &entt::locator<entt::meta_ctx>::value() != engine;
			if (m_attached) {
				entt::locator<entt::meta_ctx>::reset(layer.meta_handle());
			}
			register_content(layer, data);
		}

		void initialize_services(ds::app_data_storage& data) override { initialize_content(data.require<scn::scene_layer>(), data); }

		void shutdown_services(ds::app_data_storage& data) final
		{
			shutdown_content(data.require<scn::scene_layer>(), data);
			if (m_attached) {
				entt::locator<entt::meta_ctx>::reset();
				m_attached = false;
			}
		}

	protected:
		// Registers into the layer: layer.systems(), layer.descs(), layer.assembler(), layer.components(), and the meta
		// of its components with ecs::register_component<T>(layer.meta()). Services of the module go into `data`.
		virtual void register_content(scn::scene_layer& layer, ds::app_data_storage& data) = 0;
		virtual void initialize_content(scn::scene_layer&, ds::app_data_storage&) {}
		// Stops what runs the module's code outside the layer (tasks, subscriptions to base services).
		virtual void shutdown_content(scn::scene_layer&, ds::app_data_storage&) {}

	private:
		bool m_attached = false;
	};

	// The engine's module_layer_factory: a new scene layer of the context_manager becomes the game layer while the
	// module is loaded, and a fresh empty one replaces it afterwards. The editor does not follow a change of the game
	// layer yet (#21).
	class SNK_API game_layer_factory final : public app::modules::module_layer_factory
	{
	public:
		explicit game_layer_factory(scn::context_manager& contexts) : m_contexts(contexts) {}
		std::unique_ptr<app::modules::module_layer> create(ds::app_data_storage& root, std::string_view module) override;

	private:
		scn::context_manager& m_contexts;
	};
}
