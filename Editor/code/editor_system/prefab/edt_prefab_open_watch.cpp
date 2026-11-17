#include "edt_prefab_open_watch.h"
#include <set>
#include <string>

namespace
{
	constexpr int k_nesting_limit = 32;

	bool pending_in(const scn::prefab_desc::prefab_node& node, std::set<std::string>& seen, int depth)
	{
		for (const auto& entry : node.components) {
			const auto& handle = entry.second.parent_desc;
			if (!handle.is_valid() || handle.has_error()) continue;
			if (!handle.is_ready()) return true;
			const std::shared_ptr<desc::desc_base> loaded = handle.get();
			if (!loaded || loaded->get_type() != scn::prefab_desc::__type || depth >= k_nesting_limit) continue;
			if (!seen.emplace(std::string{ handle.get_tag().view() }).second) continue;
			if (pending_in(static_cast<const scn::prefab_desc&>(*loaded).get_root(), seen, depth + 1)) return true;
		}
		for (const auto& child : node.children) {
			if (pending_in(child, seen, depth)) return true;
		}
		return false;
	}
}

bool edt::has_pending_dependencies(const scn::prefab_desc& prefab)
{
	std::set<std::string> seen;
	return pending_in(prefab.get_root(), seen, 0);
}

void edt::prefab_open_watch::begin(res::res_handle<scn::prefab_desc> handle)
{
	m_handle = std::move(handle);
	m_active = m_handle.is_valid();
}

void edt::prefab_open_watch::cancel()
{
	m_handle = {};
	m_active = false;
}

edt::prefab_open_watch::state edt::prefab_open_watch::poll() const
{
	if (!m_active) return state::idle;
	if (m_handle.has_error()) return state::failed;
	if (!m_handle.is_ready()) return state::loading;
	const std::shared_ptr<scn::prefab_desc> loaded = m_handle.get();
	return !loaded || has_pending_dependencies(*loaded) ? state::loading : state::ready;
}
