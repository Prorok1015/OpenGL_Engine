#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_model.h>
#include <entt/entity/registry.hpp>
#include <utility>

namespace {

void release_on_destroy(entt::registry& reg, entt::entity entity)
{
	if (auto* holds = reg.ctx().find<scn::geometry_holds>()) {
		holds->release(entity);
	}
}

// Another tag in the component: the geometry it held is not drawn by this entity any more.
void release_on_update(entt::registry& reg, entt::entity entity)
{
	auto* holds = reg.ctx().find<scn::geometry_holds>();
	if (!holds) {
		return;
	}
	const auto* held = holds->find(entity);
	if (held && !(held->get_tag() == reg.get<scn::geometry_component>(entity).geom_tag)) {
		holds->release(entity);
	}
}

}

scn::geometry_holds& scn::geometry_holds::of(entt::registry& reg)
{
	if (auto* holds = reg.ctx().find<geometry_holds>()) {
		return *holds;
	}
	reg.on_destroy<scn::geometry_component>().connect<&release_on_destroy>();
	reg.on_update<scn::geometry_component>().connect<&release_on_update>();
	return reg.ctx().emplace<geometry_holds>();
}

void scn::geometry_holds::hold(entt::entity entity, res::res_handle<rnd::geometry_desc> geometry)
{
	m_held.insert_or_assign(entity, std::move(geometry));
}

void scn::geometry_holds::release(entt::entity entity)
{
	m_held.erase(entity);
}

const res::res_handle<rnd::geometry_desc>* scn::geometry_holds::find(entt::entity entity) const
{
	const auto found = m_held.find(entity);
	return found == m_held.end() ? nullptr : &found->second;
}
