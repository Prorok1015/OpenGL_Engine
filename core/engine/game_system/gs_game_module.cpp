#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/common/logger/engine_log.h>

namespace {

	// A non-owning entry: the layer owns the service.
	template<class T>
	std::shared_ptr<T> borrowed(T& object)
	{
		return std::shared_ptr<T>(std::shared_ptr<void>{}, &object);
	}

	class game_module_layer final : public app::modules::module_layer
	{
	public:
		game_module_layer(scn::context_manager& contexts, ds::app_data_storage& root)
			: m_contexts(contexts)
			, m_storage(&root)
		{
			if (auto* previous = m_contexts.game_layer()) {
				m_contexts.destroy_layer(*previous);
			}
			m_layer = &m_contexts.create_layer();
			m_contexts.set_game_layer(m_layer);
			m_storage.attach(borrowed(*m_layer));
			m_storage.attach(borrowed(m_layer->resources()));
			m_storage.attach(borrowed(m_layer->descs()));
			m_storage.attach(borrowed(m_layer->assembler()));
			m_storage.attach(borrowed(m_layer->systems()));
			m_storage.attach(borrowed(m_layer->components()));
			m_storage.attach(m_layer->hot_reload());
		}

		~game_module_layer() override
		{
			// The module's services first: they may still use the layer.
			m_storage = ds::app_data_storage{};
			m_contexts.destroy_layer(*m_layer);
			m_contexts.set_game_layer(&m_contexts.create_layer());
		}

		game_module_layer(const game_module_layer&) = delete;
		game_module_layer& operator=(const game_module_layer&) = delete;

		ds::app_data_storage& storage() override { return m_storage; }

	private:
		scn::context_manager& m_contexts;
		scn::scene_layer* m_layer = nullptr;
		ds::app_data_storage m_storage;
	};

}

std::unique_ptr<app::modules::module_layer> gs::game_layer_factory::create(ds::app_data_storage& root, std::string_view module)
{
	egLOG_INFO("app", "game layer for module '{}'", module);
	return std::make_unique<game_module_layer>(m_contexts, root);
}
