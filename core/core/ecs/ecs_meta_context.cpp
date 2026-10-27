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

ecs::meta_context_owner ecs::make_meta_context()
{
	const auto engine = entt::locator<entt::meta_ctx>::handle();
	meta_context_owner owner;
	owner.context = &entt::locator<entt::meta_ctx>::emplace();
	owner.handle = entt::locator<entt::meta_ctx>::handle();
	entt::locator<entt::meta_ctx>::reset(engine);
	return owner;
}
