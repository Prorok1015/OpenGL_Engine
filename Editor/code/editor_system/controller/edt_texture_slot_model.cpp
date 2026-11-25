#include "edt_texture_slot_model.h"
#include "edt_data_document.h"
#include "edt_reference.h"
#include <boost/json/object.hpp>
#include <array>
#include <format>
#include <utility>

namespace json = boost::json;

namespace
{
	constexpr std::array<edt::texture_slot, 3> k_material_slots{ {
		{ .index = 0, .name = "Diffuse" },
		{ .index = 1, .name = "Specular" },
		{ .index = 2, .name = "Normal" },
	} };

	const json::value* samplers_of(const json::value& shown)
	{
		const json::object* root = shown.if_object();
		return root ? root->if_contains("samplers") : nullptr;
	}
}

std::span<const edt::texture_slot> edt::material_texture_slots()
{
	return k_material_slots;
}

json::array edt::with_slot(const json::value& samplers, std::size_t index, json::value value)
{
	json::array out = samplers.is_array() ? samplers.as_array() : json::array{};
	if (out.size() <= index) {
		out.resize(index + 1);
	}
	out[index] = std::move(value);
	while (!out.empty() && out.back().is_null()) {
		out.pop_back();
	}
	return out;
}

bool edt::thumbnail_attempts::may_try(std::size_t slot, std::string_view tag, std::uint64_t revision) const
{
	const auto found = m_failed.find(slot);
	return found == m_failed.end() || found->second.tag != tag || found->second.revision != revision;
}

void edt::thumbnail_attempts::fail(std::size_t slot, std::string_view tag, std::uint64_t revision)
{
	m_failed.insert_or_assign(slot, failure{ .tag = std::string(tag), .revision = revision });
}

edt::texture_slot_state edt::texture_slot_model::state(const texture_slot& slot) const
{
	texture_slot_state out;
	const json::value shown = m_document.shown();
	const json::value* samplers = samplers_of(shown);
	if (samplers && samplers->is_array() && slot.index < samplers->as_array().size()) {
		out.value = samplers->as_array()[slot.index];
	}
	if (out.value.is_string()) {
		out.text = std::string(out.value.as_string());
	} else if (const json::object* reference = out.value.if_object()) {
		out.text = reference_path_text(*reference);
	}
	out.inherited = !out.value.is_null() && !m_document.own().contains("samplers");
	return out;
}

bool edt::texture_slot_model::set(const texture_slot& slot, json::value value)
{
	if (value.is_null()) {
		return clear(slot);
	}
	return write(slot, std::move(value), std::format("Set {} texture", slot.name));
}

bool edt::texture_slot_model::clear(const texture_slot& slot)
{
	return write(slot, json::value{}, std::format("Clear {} texture", slot.name));
}

bool edt::texture_slot_model::write(const texture_slot& slot, json::value value, std::string label)
{
	// A click, not a drag: a record of its own, apart from an edit of a field still pending.
	m_document.commit_pending();
	const bool changed = m_document.edit([&](json::value& shown) {
		if (!shown.is_object()) return false;
		json::object& root = shown.as_object();
		const json::value* current = root.if_contains("samplers");
		json::array next = with_slot(current ? *current : json::value{}, slot.index, std::move(value));
		if (current && current->is_array() && current->as_array() == next) return false;
		if ((!current || current->is_null()) && next.empty()) return false;
		root["samplers"] = std::move(next);
		return true;
	});
	m_document.commit_pending(std::move(label));
	return changed;
}
