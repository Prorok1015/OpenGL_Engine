#pragma once
#include <memory>

namespace scn { class context_manager; class context_set; class ecs_assembler; }
namespace ecs { class system_factory; }

namespace edt
{
	// The engine services the editor's controllers and documents work through. Owned by editor_system (built in init(),
	// declared before the controllers), shared by reference with the controllers, the prefab preview and the
	// editor_context. They live as long as editor_system, not as long as a document: reloading a level leaves them alone.
	// Nothing here is owned: the services belong to the app_data_storage.
	struct editor_services
	{
		std::weak_ptr<scn::context_manager> ctx_manager;
		scn::ecs_assembler* assembler = nullptr;
		ecs::system_factory* sfactory = nullptr;

		// The context set "level"; null while the context manager is gone.
		scn::context_set* level_set() const;
	};
}
