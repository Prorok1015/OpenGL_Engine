#pragma once
#include "edt_document_history.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace scn
{
	class prefab_desc;
}

namespace edt
{
	using prefab_snapshot = std::shared_ptr<const scn::prefab_desc>;

	// An edit of the prefab's working copy: the copy as it was and as it became. Consecutive records share the snapshot between them.
	class prefab_snapshot_change final : public history_change
	{
	public:
		using apply_fn = std::function<void(const prefab_snapshot&)>;

		prefab_snapshot_change(apply_fn apply, prefab_snapshot before, prefab_snapshot after);

		void undo() override { m_apply(m_before); }
		void redo() override { m_apply(m_after); }
		const std::vector<std::string>& keys() const override;

	private:
		apply_fn m_apply;
		prefab_snapshot m_before;
		prefab_snapshot m_after;
	};
}
