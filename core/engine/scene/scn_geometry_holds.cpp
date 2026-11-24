#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_model.h>
#include <snk/model_codec/res_tag.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <entt/entity/registry.hpp>
#include <utility>

namespace {

void hold_on_construct(entt::registry& reg, entt::entity entity)
{
	if (auto* holds = reg.ctx().find<scn::geometry_holds>()) {
		holds->hold(entity, reg.get<scn::geometry_component>(entity).geom_tag);
	}
}

// Another tag in the component: the entity draws that geometry now, not the one it held.
void hold_on_update(entt::registry& reg, entt::entity entity)
{
	auto* holds = reg.ctx().find<scn::geometry_holds>();
	if (!holds) {
		return;
	}
	const res::tag& tag = reg.get<scn::geometry_component>(entity).geom_tag;
	const auto* held = holds->find(entity);
	if (!held || !(held->get_tag() == tag)) {
		holds->release(entity);
		holds->hold(entity, tag);
	}
}

void release_on_destroy(entt::registry& reg, entt::entity entity)
{
	if (auto* holds = reg.ctx().find<scn::geometry_holds>()) {
		holds->release(entity);
	}
}

}

scn::geometry_holds& scn::geometry_holds::attach(entt::registry& reg, res::resource_system* resources)
{
	if (auto* holds = reg.ctx().find<geometry_holds>()) {
		ASSERT_MSG(holds->m_resources || !resources,
			"geometry_holds: the resources are fixed when the holds are made; attach them in the first call");
		return *holds;
	}
	auto& holds = reg.ctx().emplace<geometry_holds>();
	holds.m_resources = resources;
	reg.on_construct<scn::geometry_component>().connect<&hold_on_construct>();
	reg.on_update<scn::geometry_component>().connect<&hold_on_update>();
	reg.on_destroy<scn::geometry_component>().connect<&release_on_destroy>();
	return holds;
}

scn::geometry_holds::geometry_holds() = default;

scn::geometry_holds::~geometry_holds()
{
	if (m_resources) {
		for (const auto& [tag, count] : m_watching) {
			m_resources->unwatch(tag, this);
		}
	}
}

void scn::geometry_holds::watch_tag(const res::tag& tag)
{
	if (!m_resources || !tag.is_valid()) {
		return;
	}
	if (m_watching[tag]++ == 0) {
		// keep_dependencies: a change of the .bin beside the desc reaches the hold too, as it reaches the renderer's.
		// The tag the hold was made under: a rename reports the new name to the callback, the holders keep the old one.
		m_resources->watch(tag, this, [this, tag](const res::tag&) { on_changed(tag); }, true);
	}
}

void scn::geometry_holds::unwatch_tag(const res::tag& tag)
{
	const auto found = m_watching.find(tag);
	if (found == m_watching.end()) {
		return;
	}
	if (--found->second == 0) {
		m_watching.erase(found);
		m_resources->unwatch(tag, this);
	}
}

void scn::geometry_holds::on_changed(const res::tag& tag)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "geometry_holds: a reload is taken on the main thread");
	// The change evicted the block: this reads the new data, once, and every holder shares that handle.
	const auto fresh = m_resources->require<rnd::geometry_desc>(tag);
	for (auto& [entity, held] : m_held) {
		if (held.get_tag() == tag) {
			held = fresh;
		}
	}
	++m_revision;
}

std::uint64_t scn::geometry_holds::revision() const
{
	return m_revision;
}

void scn::geometry_holds::hold(entt::entity entity, res::res_handle<rnd::geometry_desc> geometry)
{
	release(entity);
	watch_tag(geometry.get_tag());
	m_held.insert_or_assign(entity, std::move(geometry));
	++m_revision;
}

void scn::geometry_holds::hold(entt::entity entity, const res::tag& geometry)
{
	// A layer's resources are asked on the main thread only; every path that makes a geometry_component in a world
	// (an assembly, a restored snapshot, a level load, a spawned prefab) runs there.
	ASSERT_MSG(!ds::task_system::on_worker(),
		"geometry_holds: a geometry_component is made or retagged on the main thread only");
	if (!m_resources || !geometry.is_valid() || !res::reference_problem(geometry).empty()) {
		return;
	}
	hold(entity, m_resources->require<rnd::geometry_desc>(geometry));
}

void scn::geometry_holds::release(entt::entity entity)
{
	const auto found = m_held.find(entity);
	if (found == m_held.end()) {
		return;
	}
	unwatch_tag(found->second.get_tag());
	m_held.erase(found);
	++m_revision;
}

const res::res_handle<rnd::geometry_desc>* scn::geometry_holds::find(entt::entity entity) const
{
	const auto found = m_held.find(entity);
	return found == m_held.end() ? nullptr : &found->second;
}

std::size_t scn::geometry_holds::size() const
{
	return m_held.size();
}
