#include <snk/ecs/ecs_meta_context.h>

entt::meta_ctx& ecs::mutable_meta_context()
{
	return entt::locator<entt::meta_ctx>::value_or();
}

const entt::meta_ctx& ecs::meta_context()
{
	return mutable_meta_context();
}

entt::locator<entt::meta_ctx>::node_type ecs::meta_context_handle()
{
	static_cast<void>(mutable_meta_context());
	return entt::locator<entt::meta_ctx>::handle();
}
