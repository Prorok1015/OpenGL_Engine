#include "spike_game_api.h"
#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_type_id.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_registration_scope.h>

#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <optional>
#include <string>
#include <string_view>

// Minimal game module of the EnTT-across-DLL spike (#218): one component, one desc type with its
// spawner, one system. It registers through the host's services the way a game.dll would.

namespace {

class velocity_desc final : public desc::desc_base
{
public:
	float dx = 0.f;

	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (const auto* value = obj.if_contains("dx")) {
			dx = static_cast<float>(value->to_number<double>());
		}
	}

	void serialize(boost::json::object& obj) const override
	{
		obj["dx"] = static_cast<double>(dx);
	}
};

void move_system(entt::view<entt::get_t<const spike::velocity_component, scn::local_transform>> view)
{
	for (auto&& [entity, velocity, transform] : view.each()) {
		transform.local[3][0] += velocity.dx;
	}
}

class spike_game_module final : public app::modules::module_interface
{
public:
	void register_services(ds::app_data_storage& data) override
	{
		auto& host = data.require<spike::host_services>();
		if (host.share_meta) {
			entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle());
		}
		ecs::register_component<spike::velocity_component>(entt::locator<entt::meta_ctx>::value_or());

		m_scope.emplace(host.descs, host.assembler);
		m_scope->desc<velocity_desc>(spike::velocity_desc_type, true);
		m_scope->spawner(spike::velocity_desc_type, [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<spike::velocity_component>(e, spike::velocity_component{ static_cast<const velocity_desc&>(base).dx });
		});

		host.systems.register_system<&move_system>(std::string{ spike::move_system_name }, { .phase = ecs::phase::update });
	}

	void initialize_services(ds::app_data_storage&) override {}

	// The system stays in the host's system_factory: system_registry has no remove yet (#220).
	void shutdown_services(ds::app_data_storage&) override
	{
		m_scope.reset();
		entt::meta_reset<spike::velocity_component>(entt::locator<entt::meta_ctx>::value_or());
		entt::locator<entt::meta_ctx>::reset();
	}

	std::string_view name() const override { return "spike_game"; }
	std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }

private:
	std::optional<scn::registration_scope> m_scope;
};

}

SNK_DECLARE_MODULE(spike_game_module)

extern "C" {

SNK_MODULE_EXPORT void spike_probe(spike::module_probe* out)
{
	out->velocity_hash = entt::type_hash<spike::velocity_component>::value();
	out->velocity_index = entt::type_index<spike::velocity_component>::value();
	out->velocity_name = entt::type_name<spike::velocity_component>::value();
	out->velocity_info = &entt::type_id<spike::velocity_component>();
	out->engine_hash = entt::type_hash<scn::local_transform>::value();
	out->engine_index = entt::type_index<scn::local_transform>::value();
	out->desc_system_key = ds::type_id::value<desc::desc_system>();
	out->meta_ctx = &entt::locator<entt::meta_ctx>::value_or();
#if defined(ENTT_STANDARD_CPP)
	out->standard_cpp = true;
#endif
}

SNK_MODULE_EXPORT void spike_spawn_in_sandbox(entt::registry* reg, float dx)
{
	auto& sandbox = reg->ctx().get<ecs::entity_spawner>();
	const auto entity = sandbox.create();
	sandbox.emplace<spike::velocity_component>(entity, spike::velocity_component{ dx });
}

SNK_MODULE_EXPORT void spike_emplace_module_only(entt::registry* reg, entt::entity entity, int value)
{
	reg->emplace<spike::module_only_component>(entity, spike::module_only_component{ value });
}

SNK_MODULE_EXPORT const void* spike_resolve_engine_type()
{
	return &entt::resolve<scn::depth_level>().info();
}

}
