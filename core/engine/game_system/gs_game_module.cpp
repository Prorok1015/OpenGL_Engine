#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/common/logger/engine_log.h>
#include <format>
#include <stdexcept>

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
		game_module_layer(scn::context_manager& contexts, ds::app_data_storage& root, ds::task_system* tasks, std::string& current,
			std::string_view module)
			: m_contexts(contexts)
			, m_tasks(tasks)
			, m_current(current)
			, m_storage(&root)
		{
			// The new layer becomes the game layer before the empty one goes, so the game layer changes once.
			m_layer = &m_contexts.create_layer(m_tasks);
			auto* previous = m_contexts.game_layer();
			m_contexts.set_game_layer(m_layer);
			if (previous) {
				m_contexts.destroy_layer(*previous);
			}
			m_storage.attach(borrowed(*m_layer));
			m_storage.attach(borrowed(m_layer->resources()));
			m_storage.attach(borrowed(m_layer->descs()));
			m_storage.attach(borrowed(m_layer->assembler()));
			m_storage.attach(borrowed(m_layer->systems()));
			m_storage.attach(borrowed(m_layer->components()));
			m_storage.attach(m_layer->hot_reload());
			m_current = module;
		}

		~game_module_layer() override
		{
			// Its tasks, its worlds (the set "level" goes with the change of the game layer), the rest of the layer,
			// then the module's services: none of them may run the module's code after the layer is gone.
			m_layer->stop_tasks();
			m_contexts.set_game_layer(&m_contexts.create_layer(m_tasks));
			m_contexts.destroy_layer(*m_layer);
			m_storage = ds::app_data_storage{};
			m_current.clear();
		}

		game_module_layer(const game_module_layer&) = delete;
		game_module_layer& operator=(const game_module_layer&) = delete;

		ds::app_data_storage& storage() override { return m_storage; }

	private:
		scn::context_manager& m_contexts;
		ds::task_system* m_tasks = nullptr;
		std::string& m_current;
		scn::scene_layer* m_layer = nullptr;
		ds::app_data_storage m_storage;
	};

}

std::unique_ptr<app::modules::module_layer> gs::game_layer_factory::create(ds::app_data_storage& root, std::string_view module)
{
	if (!m_current.empty()) {
		std::string message = std::format("game module '{}' refused: game module '{}' is loaded, and one game module at a "
			"time is supported", module, m_current);
		egLOG_ERROR("app", "{}", message);
		throw std::logic_error(std::move(message));
	}
	egLOG_INFO("app", "game layer for module '{}'", module);
	return std::make_unique<game_module_layer>(m_contexts, root, m_tasks, m_current, module);
}
