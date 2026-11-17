#include "edt_instance_overrides_view.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_node_id.h>
#include <snk/scene/level/scn_override_record.h>
#include <boost/json.hpp>
#include <imgui.h>
#include <algorithm>
#include <format>
#include <optional>

namespace
{
	std::string short_id(uint64_t id)
	{
		const std::string full = desc::format_node_id(id);
		const std::size_t first = full.find_first_not_of('0');
		return first == std::string::npos ? std::string{ "0" } : full.substr(first);
	}

	std::string value_text(const boost::json::value& value)
	{
		constexpr std::size_t longest = 48;
		std::string text = boost::json::serialize(value);
		if (text.size() > longest) {
			text.resize(longest);
			text += "...";
		}
		return text;
	}
}

std::string edt::describe_override_record(const scn::override_record& record)
{
	std::string where;
	for (const uint64_t id : record.target) {
		if (!where.empty()) where += " > ";
		where += short_id(id);
	}
	const std::string prefix = where.empty() ? std::string{} : where + " : ";

	switch (record.op) {
	case scn::override_op::add_node: {
		std::string name = "?";
		if (record.value.is_object()) {
			if (const auto* found = record.value.as_object().if_contains("name"); found && found->is_string()) {
				name = std::string(found->as_string());
			}
		}
		return std::format("{}add node '{}'", prefix, name);
	}
	case scn::override_op::remove:
		return record.component.empty() ? (where.empty() ? std::string{ "remove this node" } : std::format("remove node {}", where))
			: std::format("{}remove component {}", prefix, record.component);
	case scn::override_op::set:
		break;
	}
	return std::format("{}{} {} = {}", prefix, record.component, record.path.empty() ? std::string{ "(whole)" } : record.path, value_text(record.value));
}

std::string edt::apply_to_prefab_question(const res::tag& prefab, std::size_t instances, std::size_t records)
{
	return std::format("This changes prefab {} for all of its instances ({} in this level; other prefabs and levels may hold more).\n\n"
		"{} of this instance's overrides move into the prefab file and are removed from the instance.\n"
		"This cannot be undone: the undo history of the level is cleared.",
		prefab.view(), instances, records);
}

namespace
{
	std::vector<std::string> fingerprint(const std::vector<scn::override_record>& records)
	{
		std::vector<std::string> out;
		out.reserve(records.size());
		for (const scn::override_record& record : records) out.push_back(boost::json::serialize(scn::override_record_to_json(record)));
		return out;
	}

	constexpr const char* apply_popup = "Apply to Prefab###apply_override";

	// A question about another node, or about records that are not the ones asked about, is dropped, its window closed.
	void drop_stale_question(const scn::prefab_desc::prefab_node& node, edt::instance_overrides_state& state)
	{
		if (state.asking && (state.asked_node != node.node_id || state.asked_records != fingerprint(node.overrides))) state.asking.reset();
		if (state.asking || !ImGui::IsPopupOpen(apply_popup)) return;
		if (ImGui::BeginPopupModal(apply_popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	bool applicable(const scn::override_record& record, const edt::override_edit::stale_record* gone)
	{
		return !gone && record.op == scn::override_op::set;
	}

	std::vector<std::size_t> records_asked_about(const scn::prefab_desc::prefab_node& node, const std::vector<const edt::override_edit::stale_record*>& stale_at,
		std::size_t asking)
	{
		std::vector<std::size_t> indices;
		for (std::size_t i = 0; i < node.overrides.size(); ++i) {
			if ((asking == edt::instance_overrides_state::all_records || asking == i) && applicable(node.overrides[i], stale_at[i])) indices.push_back(i);
		}
		return indices;
	}

	bool draw_apply_question(scn::prefab_desc::prefab_node& node, const std::vector<const edt::override_edit::stale_record*>& stale_at,
		const res::tag& prefab, const edt::instance_overrides_hooks& hooks, edt::instance_overrides_state& state)
	{
		constexpr const char* popup = apply_popup;
		if (state.open_requested) {
			ImGui::OpenPopup(popup);
			state.open_requested = false;
		}
		if (!state.asking) return false;
		bool changed = false;
		bool keep_asking = true;
		if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			const std::vector<std::size_t> indices = records_asked_about(node, stale_at, *state.asking);
			const std::size_t instances = hooks.instances_in_level ? hooks.instances_in_level(prefab) : 0;
			ImGui::TextUnformatted(edt::apply_to_prefab_question(prefab, instances, indices.size()).c_str());
			ImGui::Separator();
			ImGui::BeginDisabled(indices.empty());
			const bool confirmed = ImGui::Button("Apply to Prefab");
			ImGui::EndDisabled();
			if (confirmed) {
				std::vector<scn::override_record> records;
				for (const std::size_t i : indices) records.push_back(node.overrides[i]);
				state.error = hooks.apply_to_prefab(prefab, records);
				if (state.error.empty()) {
					for (std::size_t i = indices.size(); i-- > 0;) node.overrides.erase(node.overrides.begin() + static_cast<std::ptrdiff_t>(indices[i]));
					changed = true;
				}
				keep_asking = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				keep_asking = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		} else {
			keep_asking = false;
		}
		if (!keep_asking) state.asking.reset();
		return changed;
	}
}

bool edt::draw_instance_overrides(scn::prefab_desc::prefab_node& node, const std::vector<override_edit::stale_record>* stale,
	const instance_overrides_hooks& hooks, instance_overrides_state& state)
{
	drop_stale_question(node, state);
	const scn::prefab_desc* prefab = override_edit::referenced_prefab(node);
	if (!prefab && node.overrides.empty()) return false;

	bool changed = false;
	ImGui::Separator();
	ImGui::TextUnformatted("Prefab instance");
	if (prefab) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", std::string(prefab->get_tag().view()).c_str());
		if (hooks.open_prefab && ImGui::SmallButton("Edit Prefab")) {
			hooks.open_prefab(prefab->get_tag());
		}
	}
	if (node.overrides.empty()) {
		ImGui::TextDisabled("No overrides");
		return false;
	}

	std::vector<const override_edit::stale_record*> stale_at(node.overrides.size(), nullptr);
	if (stale) {
		for (const override_edit::stale_record& entry : *stale) {
			if (entry.index < stale_at.size()) stale_at[entry.index] = &entry;
		}
	}
	const std::size_t stale_count = static_cast<std::size_t>(std::ranges::count_if(stale_at, [] (const auto* entry) { return entry != nullptr; }));

	const std::string header = std::format("Overrides ({})###instance_overrides", node.overrides.size());
	if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return false;

	const bool can_apply = prefab && hooks.apply_to_prefab && (!hooks.can_apply || hooks.can_apply());
	if (!state.error.empty()) {
		ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.f), "Not applied: %s", state.error.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Dismiss")) state.error.clear();
	}

	std::optional<std::size_t> remove_index;
	for (std::size_t i = 0; i < node.overrides.size(); ++i) {
		ImGui::PushID(static_cast<int>(i));
		const override_edit::stale_record* gone = stale_at[i];
		const std::string text = edt::describe_override_record(node.overrides[i]);
		if (gone) {
			ImGui::TextDisabled("%s", text.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("(stale: %s)", gone->what.c_str());
		} else {
			ImGui::TextUnformatted(text.c_str());
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(gone ? "Remove" : "Revert")) remove_index = i;
		if (can_apply && applicable(node.overrides[i], gone)) {
			ImGui::SameLine();
			if (ImGui::SmallButton("Apply to Prefab")) {
				state.asking = i;
				state.open_requested = true;
				state.asked_node = node.node_id;
				state.asked_records = fingerprint(node.overrides);
			}
		}
		ImGui::PopID();
	}
	const res::tag prefab_tag = prefab ? prefab->get_tag() : res::tag{};
	bool changed_by_apply = can_apply && draw_apply_question(node, stale_at, prefab_tag, hooks, state);
	if (changed_by_apply) return true;
	if (remove_index) {
		node.overrides.erase(node.overrides.begin() + static_cast<std::ptrdiff_t>(*remove_index));
		return true;
	}

	if (ImGui::SmallButton("Revert All")) {
		node.overrides.clear();
		return true;
	}
	if (can_apply && !records_asked_about(node, stale_at, instance_overrides_state::all_records).empty()) {
		ImGui::SameLine();
		if (ImGui::SmallButton("Apply All to Prefab")) {
			state.asking = instance_overrides_state::all_records;
			state.open_requested = true;
			state.asked_node = node.node_id;
			state.asked_records = fingerprint(node.overrides);
		}
	}
	if (stale_count > 0) {
		ImGui::SameLine();
		const std::string label = std::format("Clear stale overrides ({})", stale_count);
		if (ImGui::SmallButton(label.c_str())) {
			for (std::size_t i = stale_at.size(); i-- > 0;) {
				if (stale_at[i]) node.overrides.erase(node.overrides.begin() + static_cast<std::ptrdiff_t>(i));
			}
			changed = true;
		}
	}
	return changed;
}
