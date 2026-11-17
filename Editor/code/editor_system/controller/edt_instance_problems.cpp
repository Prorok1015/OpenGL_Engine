#include "edt_instance_problems.h"
#include "edt_instance_overrides_view.h"
#include "edt_override_edit.h"
#include <snk/desc/desc_node_id.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

namespace
{
	using prefab_node = scn::prefab_desc::prefab_node;

	std::string child_pointer(const std::string& pointer, std::size_t index)
	{
		return std::format("{}/children/{}", pointer, index);
	}

	std::string shown_name(const prefab_node& node)
	{
		return node.name.empty() ? desc::format_node_id(node.node_id) : node.name;
	}

	// The prefabs that instantiate themselves below one prefab, walked from it alone: what a node says depends on its own prefab, not
	// on what was walked for another node before it. One text per cycle, in the order the walk finds them.
	class cycle_finder
	{
	public:
		const std::vector<std::string>& from(const scn::prefab_desc& start)
		{
			const std::string key{ start.get_tag().view() };
			if (const auto known = m_by_start.find(key); known != m_by_start.end()) return known->second;
			m_stack.clear();
			m_done.clear();
			m_found.clear();
			enter(start);
			return m_by_start.emplace(key, std::move(m_found)).first->second;
		}

	private:
		void enter(const scn::prefab_desc& prefab)
		{
			const std::string key{ prefab.get_tag().view() };
			m_stack.push_back(key);
			visit(prefab.get_root());
			m_stack.pop_back();
			m_done.insert(key);
		}

		void found_cycle(const std::string& target)
		{
			const auto at = std::ranges::find(m_stack, target);
			if (at == m_stack.end()) return;
			if (at + 1 == m_stack.end()) {
				m_found.push_back(std::format("Prefab '{}' refers to itself", target));
				return;
			}
			std::string chain;
			for (auto it = at; it != m_stack.end(); ++it) chain += *it + " -> ";
			m_found.push_back(std::format("Prefab '{}' refers to itself through '{}' ({}{})", target, m_stack.back(), chain, target));
		}

		void visit(const prefab_node& node)
		{
			if (const scn::prefab_desc* nested = edt::override_edit::referenced_prefab(node)) {
				const std::string key{ nested->get_tag().view() };
				if (std::ranges::find(m_stack, key) != m_stack.end()) found_cycle(key);
				else if (!m_done.contains(key)) enter(*nested);
			}
			// A reference to the prefab's own file is refused when it loads (#952): the handle holds the error and the tag it named.
			for (const auto& entry : node.components) {
				const auto& handle = entry.second.parent_desc;
				if (!handle.is_valid() || !handle.has_error()) continue;
				if (const res::tag named = res::refused_tag(handle); named.is_valid()) found_cycle(std::string{ named.view() });
			}
			for (const prefab_node& child : node.children) visit(child);
		}

		std::vector<std::string> m_stack;
		std::set<std::string> m_done;
		std::vector<std::string> m_found;
		std::map<std::string, std::vector<std::string>> m_by_start;
	};

	struct collector
	{
		const std::string& file;
		std::vector<res::diagnostic>& out;
		cycle_finder& cycles;
		std::set<std::tuple<std::string, std::string, std::string>>& reported;

		void walk(const prefab_node& node, const std::string& pointer)
		{
			for (const scn::override_record& record : node.overrides) {
				if (record.op == scn::override_op::set) continue;
				out.push_back({ res::diagnostic_severity::warning, "override", file, pointer,
					std::format("node '{}': the override \"{}\" is not made by the editor and cannot be changed here; it stays in the file and is applied as written",
						shown_name(node), edt::describe_override_record(record)) });
			}
			if (const scn::prefab_desc* prefab = edt::override_edit::referenced_prefab(node)) {
				for (const std::string& text : cycles.from(*prefab)) {
					if (reported.emplace(file, pointer, text).second) {
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
	std::set<std::tuple<std::string, std::string, std::string>> reported;
	for (const instance_world& entry : worlds) {
		if (!entry.world) continue;
		collector{ entry.file, out, cycles, reported }.walk(entry.world->get_root(), entry.pointer_prefix);
	}
	return out;
}
