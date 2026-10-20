#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace {

struct item
{
	const ecs::system_entry* entry = nullptr;
	size_t phase = 0;
};

std::string join(const std::vector<std::string>& parts, std::string_view separator)
{
	std::string out;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i) out += separator;
		out += parts[i];
	}
	return out;
}

} // namespace

size_t ecs::system_plan::system_count() const noexcept
{
	return std::accumulate(phases.begin(), phases.end(), size_t{ 0 },
		[](size_t n, const phase_plan& p) { return n + p.nodes.size(); });
}

bool ecs::system_plan::has_errors() const noexcept
{
	return std::ranges::any_of(diagnostics, &plan_diagnostic::is_error);
}

bool ecs::system_plan::has_diagnostic(diagnostic_kind kind) const noexcept
{
	return std::ranges::any_of(diagnostics, [kind](const plan_diagnostic& d) { return d.kind == kind; });
}

const ecs::phase_plan* ecs::system_plan::find_phase(const phase_id& id) const noexcept
{
	auto it = std::ranges::find_if(phases, [&](const phase_plan& p) { return p.id == id; });
	return it == phases.end() ? nullptr : &*it;
}

std::vector<std::string> ecs::system_plan::names_in(const phase_id& id) const
{
	std::vector<std::string> out;
	if (const phase_plan* p = find_phase(id)) {
		for (const auto& node : p->nodes) out.push_back(node.name);
	}
	return out;
}

ecs::system_plan ecs::build_system_plan(
	const system_registry& registry,
	std::span<const std::string> names,
	plan_options options)
{
	system_plan plan;
	auto report = [&](diagnostic_kind kind, std::string message) {
		const bool is_error = plan_diagnostic{ kind, {} }.is_error();
		if (is_error) egLOG_ERROR("ecs", "{}", message);
		else egLOG_WARN("ecs", "{}", message);
		plan.diagnostics.push_back({ kind, std::move(message) });
	};

	const std::vector<phase_id> ordered_phases = registry.phases().ordered();
	std::unordered_map<std::string, size_t> phase_index;
	plan.phases.reserve(ordered_phases.size());
	for (const auto& id : ordered_phases) {
		phase_index.emplace(id.name(), plan.phases.size());
		plan.phases.push_back(phase_plan{ id, {} });
	}

	// Items keep the order of `names`; that order is the tie-break.
	std::vector<item> items;
	std::unordered_map<std::string, size_t> item_by_name;
	items.reserve(names.size());
	for (const std::string& name : names) {
		const system_entry* entry = registry.find(name);
		if (!entry) {
			report(diagnostic_kind::unknown_system, std::format("unknown system '{}' in the world's system list, skipped", name));
			continue;
		}
		if (item_by_name.contains(name)) {
			report(diagnostic_kind::duplicate_system, std::format("system '{}' listed more than once, extra entry skipped", name));
			continue;
		}
		auto phase_it = phase_index.find(entry->desc.phase.name());
		if (phase_it == phase_index.end()) {
			report(diagnostic_kind::unknown_phase,
				std::format("system '{}' uses phase '{}' that was never added, skipped", name, entry->desc.phase.name()));
			continue;
		}
		item_by_name.emplace(name, items.size());
		items.push_back(item{ entry, phase_it->second });
	}

	std::vector<std::vector<size_t>> successors(items.size());
	std::vector<size_t> in_degree(items.size(), 0);
	auto add_edge = [&](size_t from, size_t to) {
		auto& out = successors[from];
		if (std::ranges::find(out, to) != out.end()) return;
		out.push_back(to);
		++in_degree[to];
	};

	auto resolve = [&](size_t self, const std::string& target, bool target_runs_first) {
		const std::string& self_name = items[self].entry->name;
		if (target == self_name) {
			report(diagnostic_kind::cycle, std::format("system '{}' is ordered relative to itself", self_name));
			return;
		}
		if (!registry.find(target)) {
			report(diagnostic_kind::unknown_constraint_target,
				std::format("system '{}' is ordered {} unknown system '{}', constraint ignored",
					self_name, target_runs_first ? "after" : "before", target));
			return;
		}
		auto it = item_by_name.find(target);
		if (it == item_by_name.end()) return; // registered, but this world does not run it
		const size_t other = it->second;
		if (items[other].phase == items[self].phase) {
			if (target_runs_first) add_edge(other, self);
			else add_edge(self, other);
			return;
		}
		const bool consistent = target_runs_first ? items[other].phase < items[self].phase : items[self].phase < items[other].phase;
		if (!consistent) {
			report(diagnostic_kind::wrong_phase_constraint,
				std::format("system '{}' (phase '{}') is ordered {} '{}' (phase '{}'), which contradicts the phase order; constraint ignored",
					self_name, items[self].entry->desc.phase.name(), target_runs_first ? "after" : "before",
					target, items[other].entry->desc.phase.name()));
		}
	};

	for (size_t i = 0; i < items.size(); ++i) {
		for (const auto& target : items[i].entry->desc.after) resolve(i, target, true);
		for (const auto& target : items[i].entry->desc.before) resolve(i, target, false);
	}

	std::vector<size_t> emitted_order;
	emitted_order.reserve(items.size());
	std::vector<bool> emitted(items.size(), false);

	for (size_t p = 0; p < plan.phases.size(); ++p) {
		std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
		size_t in_phase = 0;
		for (size_t i = 0; i < items.size(); ++i) {
			if (items[i].phase != p) continue;
			++in_phase;
			if (in_degree[i] == 0) ready.push(i);
		}

		size_t done = 0;
		while (!ready.empty()) {
			const size_t i = ready.top();
			ready.pop();
			emitted[i] = true;
			emitted_order.push_back(i);
			++done;
			for (size_t next : successors[i]) {
				if (--in_degree[next] == 0) ready.push(next);
			}
		}
		if (done == in_phase) continue;

		// What is left contains a cycle (and whatever hangs off it). Walk predecessors to name it.
		std::vector<size_t> remaining;
		for (size_t i = 0; i < items.size(); ++i) {
			if (items[i].phase == p && !emitted[i]) remaining.push_back(i);
		}
		auto predecessor_of = [&](size_t node) {
			for (size_t cand : remaining) {
				if (std::ranges::find(successors[cand], node) != successors[cand].end()) return cand;
			}
			return node;
		};
		std::vector<size_t> walk;
		size_t cur = remaining.front();
		while (std::ranges::find(walk, cur) == walk.end()) {
			walk.push_back(cur);
			cur = predecessor_of(cur);
		}
		std::vector<std::string> cycle_names;
		for (auto it = std::ranges::find(walk, cur); it != walk.end(); ++it) cycle_names.push_back(items[*it].entry->name);
		std::ranges::reverse(cycle_names); // predecessors were walked backwards
		cycle_names.push_back(cycle_names.front());
		report(diagnostic_kind::cycle,
			std::format("ordering cycle in phase '{}': {}; falling back to list order for the affected systems",
				plan.phases[p].id.name(), join(cycle_names, " -> ")));

		for (size_t i : remaining) {
			emitted[i] = true;
			emitted_order.push_back(i);
		}
	}

	for (size_t i : emitted_order) {
		const system_entry& entry = *items[i].entry;
		system_node node;
		node.name = entry.name;
		node.run_in_edit = entry.desc.run_in_edit;
		node.run = entry.run;
		node.instance = entry.make ? entry.make() : system_instance{};
		plan.phases[items[i].phase].nodes.push_back(std::move(node));
	}

	if (options.validate_access) {
		// Two systems of one phase that touch the same data, at least one of them writing, must be
		// ordered (directly or through a chain). Otherwise the result depends on the list order.
		auto reaches = [&](size_t from, size_t to) {
			std::vector<bool> seen(items.size(), false);
			std::vector<size_t> stack{ from };
			while (!stack.empty()) {
				const size_t cur = stack.back();
				stack.pop_back();
				for (size_t next : successors[cur]) {
					if (next == to) return true;
					if (!seen[next]) {
						seen[next] = true;
						stack.push_back(next);
					}
				}
			}
			return false;
		};

		for (size_t a = 0; a < items.size(); ++a) {
			for (size_t b = a + 1; b < items.size(); ++b) {
				if (items[a].phase != items[b].phase) continue;
				if (reaches(a, b) || reaches(b, a)) continue;

				std::vector<std::string> both_write;
				std::vector<std::string> one_writes;
				for (const auto& ea : items[a].entry->access) {
					for (const auto& eb : items[b].entry->access) {
						if (ea.type != eb.type || !(ea.write || eb.write)) continue;
						std::string label{ ea.name };
						auto& bucket = (ea.write && eb.write) ? both_write : one_writes;
						if (std::ranges::find(bucket, label) == bucket.end()) bucket.push_back(std::move(label));
					}
				}

				const std::string& name_a = items[a].entry->name;
				const std::string& name_b = items[b].entry->name;
				const std::string& phase_name = plan.phases[items[a].phase].id.name();
				if (!both_write.empty()) {
					report(diagnostic_kind::access_conflict,
						std::format("systems '{}' and '{}' both write {} in phase '{}' with no order; add after/before",
							name_a, name_b, join(both_write, ", "), phase_name));
				}
				if (!one_writes.empty()) {
					report(diagnostic_kind::access_conflict,
						std::format("systems '{}' and '{}' read and write {} in phase '{}' with no order; add after/before",
							name_a, name_b, join(one_writes, ", "), phase_name));
				}
			}
		}
	}

	return plan;
}
