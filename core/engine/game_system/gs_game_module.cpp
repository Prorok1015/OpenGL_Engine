#include <snk/game/gs_game_module.h>
#include <snk/command/cmd_registry.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/common/logger/engine_log_sinks.h>
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
			, m_storage(&root, ds::app_data_storage::isolated)
		{
			// The game layer moves to it in activate(), once the module has registered: the worlds built then see the
			// module's systems and types.
			m_layer = &m_contexts.create_layer(m_tasks);
			m_layer->resources().limit_writes_to(gs::module_resource_prefix(module));
			m_storage.attach(borrowed(*m_layer));
			m_storage.attach(borrowed(m_layer->resources()));
			m_storage.attach(borrowed(m_layer->descs()));
			m_storage.attach(borrowed(m_layer->assembler()));
			m_storage.attach(borrowed(m_layer->systems()));
			m_storage.attach(borrowed(m_layer->components()));
			// Borrowed like the rest: the layer destroys its hot reload manager while its resources are still there.
			m_storage.attach(borrowed(*m_layer->hot_reload()));
			m_storage.construct<gs::module_context>(*m_layer, root.require_shared<cmd::command_registry>().get(), m_tasks);
			m_current = module;
		}

		~game_module_layer() override
		{
			// Its tasks, its worlds (the set "level" goes with the change of the game layer), the rest of the layer,
			// then the module's services: none of them may run the module's code after the layer is gone.
			m_layer->stop_tasks();
			// Stopping them queued the main-thread continuations of the module's tasks (then_main, with an owner or
			// without): they run now, while the layer is there, so none of them holds the module's code past its unload
			// (#406). Only those: the rest of the main queue waits for its frame.
			ds::detail::run_pending_main(m_layer->tasks());
			if (m_active) {
				m_contexts.set_game_layer(&m_contexts.create_layer(m_tasks));
			}
			m_contexts.destroy_layer(*m_layer);
			m_storage = ds::app_data_storage{};
			m_current.clear();
		}

		// From inside one of the module's callbacks (a continuation of its tasks, or a callback held by layer.tasks())
		// the unload would destroy the code that runs it.
		std::string unload_refusal() const override
		{
			if (ds::detail::running_callbacks(m_layer->tasks()) > 0) {
				return "called from inside one of the module's callbacks; unload from a main-queue item of the engine "
					"(task_system::post_main) instead";
			}
			return {};
		}

		// The new layer becomes the game layer before the empty one goes, so the game layer changes once.
		void activate() override
		{
			auto* previous = m_contexts.game_layer();
			// Before the swap: a throwing layer-change callback leaves m_layer as the game layer, and the destructor
			// must then put a fresh one in its place.
			m_active = true;
			m_contexts.set_game_layer(m_layer);
			if (previous) {
				m_contexts.destroy_layer(*previous);
			}
		}

		game_module_layer(const game_module_layer&) = delete;
		game_module_layer& operator=(const game_module_layer&) = delete;

		ds::app_data_storage& storage() override { return m_storage; }

	private:
		scn::context_manager& m_contexts;
		ds::task_system* m_tasks = nullptr;
		std::string& m_current;
		scn::scene_layer* m_layer = nullptr;
		bool m_active = false;
		ds::app_data_storage m_storage;
	};

}

bool gs::module_context::add_command(cmd::command_desc desc)
{
	if (!m_commands) {
		egLOG_ERROR("app", "module_context: no command registry, command '{}' not added", desc.id);
		return false;
	}
	cmd::registration added = m_commands->add(std::move(desc));
	if (!added.active()) {
		return false;
	}
	m_layer.keep(std::move(added));
	return true;
}

ds::task_handle<void> gs::module_context::run(ds::lane lane, ds::task_desc desc, std::function<void(ds::task_context&)> fn)
{
	ASSERT_MSG(m_tasks, "module_context::run: the layer has no task system");
	if (!m_tasks) {
		return {};
	}
	return m_tasks->run(lane, std::move(desc), m_layer.tasks(), std::move(fn));
}

std::string gs::module_resource_prefix(std::string_view module)
{
	return std::format("memory://modules/{}/", module);
}

void gs::module_context::add_log_sink(engine::log_callback_t callback)
{
	m_layer.keep(engine::add_log_sink(std::move(callback)));
}

std::unique_ptr<app::modules::module_layer> gs::game_layer_factory::create(ds::app_data_storage& root, std::string_view module)
{
	if (!m_current.empty()) {
		std::string message = std::format("game module '{}' refused: game module '{}' is loaded, and one game module at a "
			"time is supported", module, m_current);
		// Logged by whoever loads the module (core::load_project_modules), like the loader's own refusals.
		throw std::logic_error(std::move(message));
	}
	egLOG_INFO("app", "game layer for module '{}'", module);
	return std::make_unique<game_module_layer>(m_contexts, root, m_tasks, m_current, module);
}
