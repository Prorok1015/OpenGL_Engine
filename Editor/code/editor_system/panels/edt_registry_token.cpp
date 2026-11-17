#include "edt_registry_token.h"
#include <atomic>
#include <entt/entt.hpp>

namespace edt
{
	namespace {
		// The editor's own type in a registry's ctx (never one of the engine's).
		struct registry_mark
		{
			std::uint64_t id = 0;
		};
	}

	std::uint64_t registry_token(entt::registry& reg)
	{
		static std::atomic<std::uint64_t> last{ 0 };
		if (const registry_mark* mark = reg.ctx().find<registry_mark>()) return mark->id;
		return reg.ctx().emplace<registry_mark>(registry_mark{ ++last }).id;
	}
}
