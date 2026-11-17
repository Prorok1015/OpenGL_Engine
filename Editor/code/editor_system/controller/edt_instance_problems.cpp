#include "edt_instance_problems.h"
#include "edt_instance_overrides_view.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_node_id.h>
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string_view>

namespace
{
	using prefab_node = scn::prefab_desc::prefab_node;

	std::string child_pointer(std::string pointer, std::size_t index)
	{
		return std::format("{}/children/{}", pointer, index);
	}

	std::string shown_name(const prefab_node& node)
	{
		return node.name.empty() ? desc::format_node_id(node.node_id) : node.name;
	}

	std::string join_chain(const std::vector<std::string>& chain, std::size_t from)
	{
		std::string text;
		for (std::size_t i = from; i < chain.size(); ++i) {
			if (!text.empty()) text += " -> ";
			text += chain[i];
		}
		return text;
	}

	// Finds the prefabs that instantiate themselves below `prefab`; each chain is a text, found once.
	class cycle_finder
	{
	public:
		const std::vector<std::string>& cycles_below(const scn::prefab_desc& prefab)
		{
			const std::string key{ prefab.get_tag().view() };
			if (const auto known = m_known.find(key); known != m_known.end()) return known->second;
			if (std::ranges::find(m_stack, key) != m_stack.end()) return m_none;
			m_stack.push_back(key);
			std::vector<std::string> found;
			visit(prefab.get_root(), found);
			m_stack.pop_back();
			return m_known.emplace(key, std::move(found)).first->second;
		}

	private:
		void visit(const prefab_node& node, std::vector<std::string>& found)
		{
			if (const scn::prefab_desc* nested = edt::override_edit::referenced_prefab(node)) {
				const std::string key{ nested->get_tag().view() };
				if (const auto at = std::ranges::find(m_stack, key); at != m_stack.end()) {
					const std::size_t from = static_cast<std::size_t>(at - m_stack.begin());
					found.push_back(m_stack.size() - from == 1 ? std::format("Prefab '{}' refers to itself", key)
						: std::format("Prefab '{}' refers to itself through '{}' ({} -> {})", key, m_stack.back(), join_chain(m_stack, from), key));
				} else {
					for (const std::string& text : cycles_below(*nested)) found.push_back(text);
				}
			}
			for (const prefab_node& child : node.children) visit(child, found);
		}

		std::vector<std::string> m_stack;
		std::map<std::string, std::vector<std::string>> m_known;
		const std::vector<std::string> m_none;
	};

	struct collector
	{
		const std::string& file;
		std::vector<res::diagnostic>& out;
		cycle_finder& cycles;
		std::set<std::string>& reported;

		void walk(const prefab_node& node, const std::string& pointer)
		{
			for (const scn::override_record& record : node.overrides) {
				if (record.op == scn::override_op::set) continue;
				out.push_back({ res::diagnostic_severity::warning, "override", file, pointer,
					std::format("node '{}': the override \"{}\" is not made by the editor and cannot be changed here; it stays in the file and is applied as written",
						shown_name(node), edt::describe_override_record(record)) });
			}
			if (const scn::prefab_desc* prefab = edt::override_edit::referenced_prefab(node)) {
				for (const std::string& text : cycles.cycles_below(*prefab)) {
					if (reported.insert(text).second) {
						out.push_back({ res::diagnostic_severity::error, std::string(res::diagnostic_kind::cycle), file, pointer, text });
					}
				}
			}
			for (std::size_t i = 0; i < node.children.size(); ++i) walk(node.children[i], child_pointer(pointer, i));
		}
	};
}

std::vector<res::diagnostic> edt::instance_diagnostics(std::span<const instance_world> worlds)
{
	std::vector<res::diagnostic> out;
	cycle_finder cycles;
	std::set<std::string> reported;
	for (const instance_world& entry : worlds) {
		if (!entry.world) continue;
		collector{ entry.file, out, cycles, reported }.walk(entry.world->get_root(), {});
	}
	return out;
}
