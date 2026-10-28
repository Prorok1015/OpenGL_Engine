#pragma once
#include <memory>

namespace scn { class context_manager; class context_set; class ecs_assembler; class scene_layer; }
namespace ecs { class system_factory; }
namespace desc { class desc_system; }
namespace res { class resource_system; }

namespace edt
{
	// The engine services the editor's controllers and documents work through. Owned by editor_system (built in init(),
	// declared before the controllers), shared by reference with the controllers, the prefab preview and the
	// editor_context. They live as long as editor_system, not as long as a document: reloading a level leaves them alone.
	// Nothing here is owned: the services belong to the app_data_storage and to the context manager's layers.
	//
	// The game worlds belong to the context manager's game layer (MODULE_LAYER.md), which is replaced when a game module
	// loads or unloads. Take the game services from the accessors below at the moment of use and never keep the
	// references: a reference taken earlier may belong to a destroyed layer. The base_* services are the ones of the
	// app_data_storage, used when there is no game layer (tests that build the editor on the base services alone).
	struct editor_services
	{
		std::weak_ptr<scn::context_manager> ctx_manager;
		desc::desc_system* base_descs = nullptr;
		res::resource_system* base_resources = nullptr;
		scn::ecs_assembler* base_assembler = nullptr;
		ecs::system_factory* base_sfactory = nullptr;

		// The context set "level"; null while the context manager is gone.
		scn::context_set* level_set() const;
		// The layer of the game worlds; null without one (or without the context manager).
		scn::scene_layer* active_layer() const;

		// The active layer's services, else the base ones; null only when neither is set.
		desc::desc_system* descs() const;
		res::resource_system* resources() const;
		scn::ecs_assembler* assembler() const;
		ecs::system_factory* sfactory() const;
	};
}
