#include "edt_world_systems_model.h"
#include <boost/json.hpp>
#include <cstdint>
#include <utility>

namespace json = boost::json;

namespace {
	std::string text_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string();
	}

	bool flag_of(const json::object& object, std::string_view key, bool fallback)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_bool() ? value->as_bool() : fallback;
	}

	std::vector<edt::world_systems_model::item> items_of(const json::object& listing, std::string_view key)
	{
		std::vector<edt::world_systems_model::item> out;
		const json::value* array = listing.if_contains(key);
		if (!array || !array->is_array()) return out;
		for (const json::value& entry : array->as_array()) {
			if (!entry.is_object()) continue;
			const json::object& object = entry.as_object();
			out.push_back({ text_of(object, "name"), flag_of(object, "known", true), text_of(object, "phase"), flag_of(object, "run_in_edit", false) });
		}
		return out;
	}
}

edt::world_systems_model::world_systems_model(editor_events& events, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	m_document = m_events.document_changed.subscribe([this](document_id, std::uint64_t) { m_dirty = true; });
	m_active_world = m_events.active_world_changed.subscribe([this](document_id) { m_dirty = true; });
}

void edt::world_systems_model::refresh()
{
	m_dirty = false;
	m_systems.clear();
	m_available.clear();
	m_world.clear();
	if (!m_ports.invoke) return;
	const cmd::result listing = m_ports.invoke("world.systems.list", json::object{});
	if (!listing.ok || !listing.value.is_object()) {
		m_list_error = listing.error;
		return;
	}
	m_list_error.clear();
	const json::object& object = listing.value.as_object();
	m_world = text_of(object, "world");
	m_systems = items_of(object, "systems");
	m_available = items_of(object, "available");
}

void edt::world_systems_model::refresh_if_changed()
{
	if (m_dirty) refresh();
}

bool edt::world_systems_model::run(std::string_view id, json::object args)
{
	const cmd::result result = m_ports.invoke ? m_ports.invoke(id, std::move(args)) : cmd::result::failure(cmd::codes::not_found, "no commands");
	m_error = result.ok ? std::string() : result.error;
	refresh();
	return result.ok;
}

bool edt::world_systems_model::add(const std::string& name)
{
	return run("world.systems.add", { { "world", json::string(m_world) }, { "system", json::string(name) } });
}

bool edt::world_systems_model::remove(const std::string& name)
{
	return run("world.systems.remove", { { "world", json::string(m_world) }, { "system", json::string(name) } });
}

bool edt::world_systems_model::move(const std::string& name, std::size_t to)
{
	return run("world.systems.move", { { "world", json::string(m_world) }, { "system", json::string(name) }, { "to", static_cast<std::int64_t>(to) } });
}
