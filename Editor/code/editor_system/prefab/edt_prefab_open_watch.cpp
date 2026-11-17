#include "edt_prefab_open_watch.h"
#include <utility>

namespace
{
	constexpr int k_nesting_limit = 32;

	struct walk
	{
		std::set<std::string> seen;
		std::set<std::string>* settled;
		bool* too_deep;
	};

	bool pending_in(const scn::prefab_desc::prefab_node& node, walk& state, int depth)
	{
		for (const auto& entry : node.components) {
			const auto& handle = entry.second.parent_desc;
			if (!handle.is_valid() || handle.has_error()) continue;
			if (!handle.is_ready()) return true;
			const std::shared_ptr<desc::desc_base> loaded = handle.get();
			if (!loaded || loaded->get_type() != scn::prefab_desc::__type) continue;
			if (depth >= k_nesting_limit) {
				if (state.too_deep) *state.too_deep = true;
				continue;
			}
			const std::string tag{ handle.get_tag().view() };
			if (state.settled && state.settled->contains(tag)) continue;
			if (!state.seen.emplace(tag).second) continue;
			if (pending_in(static_cast<const scn::prefab_desc&>(*loaded).get_root(), state, depth + 1)) return true;
			if (state.settled) state.settled->insert(tag);
		}
		for (const auto& child : node.children) {
			if (pending_in(child, state, depth)) return true;
		}
		return false;
	}
}

bool edt::has_pending_dependencies(const scn::prefab_desc& prefab, std::set<std::string>* settled, bool* too_deep)
{
	walk state{ {}, settled, too_deep };
	return pending_in(prefab.get_root(), state, 0);
}

void edt::prefab_open_watch::begin(res::res_handle<scn::prefab_desc> handle, clock::time_point now)
{
	m_handle = std::move(handle);
	m_active = m_handle.is_valid();
	m_too_deep = false;
	m_started = now;
	m_settled.clear();
}

void edt::prefab_open_watch::cancel()
{
	m_handle = {};
	m_active = false;
	m_settled.clear();
}

res::res_handle<scn::prefab_desc> edt::prefab_open_watch::take()
{
	res::res_handle<scn::prefab_desc> out = std::move(m_handle);
	m_handle = {};
	m_active = false;
	m_settled.clear();
	return out;
}

edt::prefab_open_watch::state edt::prefab_open_watch::poll(clock::time_point now)
{
	if (!m_active) return state::idle;
	if (m_handle.has_error()) return state::failed;
	if (m_handle.is_ready()) {
		const std::shared_ptr<scn::prefab_desc> loaded = m_handle.get();
		if (!loaded) return state::failed;
		if (!has_pending_dependencies(*loaded, &m_settled, &m_too_deep)) return state::ready;
	}
	return now - m_started >= k_deadline ? state::timed_out : state::loading;
}
