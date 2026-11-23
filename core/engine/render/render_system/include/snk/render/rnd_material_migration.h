#pragma once
#include <snk/common/snk_api.h>
#include <boost/json/object.hpp>

namespace desc { class migration_context; }

namespace rnd
{
	// material_desc v1 -> v2 (#1200): the stage tags become a reference to one of the engine's .shader.desc files. A
	// material that sets no stage keeps inheriting. Stages the engine has no shader for, or that only a parent completes
	// (and the parent is not the engine's base material, as context.id_of tells), refuse the load: the project migration
	// writes a .shader.desc for them.
	SNK_API void migrate_material_v1(boost::json::object& material, desc::migration_context& context);
}
