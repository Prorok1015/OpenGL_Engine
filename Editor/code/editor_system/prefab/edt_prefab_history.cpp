#include "edt_prefab_history.h"
#include <utility>

namespace edt
{
	prefab_snapshot_change::prefab_snapshot_change(apply_fn apply, prefab_snapshot before, prefab_snapshot after)
		: m_apply(std::move(apply)), m_before(std::move(before)), m_after(std::move(after))
	{
	}

	const std::vector<std::string>& prefab_snapshot_change::keys() const
	{
		static const std::vector<std::string> keys{ "prefab" };
		return keys;
	}
}
