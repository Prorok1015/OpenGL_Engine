#include "edt_instance_overrides_view.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_node_id.h>
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

bool edt::draw_instance_overrides(scn::prefab_desc::prefab_node& node, const std::vector<override_edit::stale_record>* stale,
	const instance_overrides_hooks& hooks)
{
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
		ImGui::PopID();
	}
	if (remove_index) {
		node.overrides.erase(node.overrides.begin() + static_cast<std::ptrdiff_t>(*remove_index));
		return true;
	}

	if (ImGui::SmallButton("Revert All")) {
		node.overrides.clear();
		return true;
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
