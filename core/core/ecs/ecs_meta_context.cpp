#include <snk/ecs/ecs_component.h>

entt::meta_ctx& ecs::meta_context()
{
	return entt::locator<entt::meta_ctx>::value_or();
}

entt::locator<entt::meta_ctx>::node_type ecs::meta_context_handle()
{
	static_cast<void>(meta_context());
	return entt::locator<entt::meta_ctx>::handle();
}

