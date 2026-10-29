#include <snk/editor/edt_document_history.h>
#include <algorithm>
#include <utility>

void edt::document_history::set_limit(std::size_t limit)
{
	m_limit = limit == 0 ? 1 : limit;
	enforce_limit();
}

std::string edt::document_history::origin_key(const cmd::origin& origin)
{
	return std::to_string(static_cast<int>(origin.kind)) + ":" + origin.session;
}

void edt::document_history::drop_redo()
{
	if (m_saved && std::any_of(m_records.begin() + static_cast<std::ptrdiff_t>(m_position), m_records.end(),
			[&](const history_record& r) { return r.id == *m_saved; })) {
		m_saved.reset(); // the saved state was undone and is gone now
	}
	m_records.erase(m_records.begin() + static_cast<std::ptrdiff_t>(m_position), m_records.end());
}

void edt::document_history::enforce_limit()
{
	while (m_records.size() > m_limit && m_position > 0) {
		// The saved state stays reachable only while its record is kept, or when nothing below it is left.
		if (m_saved && *m_saved == m_records.front().id) {
			m_saved.reset();
		}
		m_records.erase(m_records.begin());
		--m_position;
	}
}

void edt::document_history::record(std::string label, const cmd::origin& origin, std::unique_ptr<history_change> change)
{
	if (!change) {
		return;
	}
	drop_redo();
	const auto group = m_groups.find(origin_key(origin));
	if (group != m_groups.end() && m_position > 0 && m_records[m_position - 1].group == group->second.id) {
		// Nobody else edited since the last edit of the group: one record.
		auto& top = m_records[m_position - 1];
		top.changes.push_back(std::move(change));
		if (m_saved && *m_saved == top.id) {
			m_saved.reset(); // the record changed after it was saved
		}
		return;
	}
	history_record entry;
	entry.id = m_next_id++;
	entry.origin = origin;
	entry.changes.push_back(std::move(change));
	if (group != m_groups.end()) {
		entry.group = group->second.id;
		entry.label = group->second.label;
	} else {
		entry.label = std::move(label);
	}
	m_records.push_back(std::move(entry));
	++m_position;
	enforce_limit();
}

bool edt::document_history::undo()
{
	if (!can_undo()) {
		return false;
	}
	auto& entry = m_records[m_position - 1];
	for (auto it = entry.changes.rbegin(); it != entry.changes.rend(); ++it) {
		(*it)->undo();
	}
	--m_position;
	return true;
}

bool edt::document_history::redo()
{
	if (!can_redo()) {
		return false;
	}
	for (auto& change : m_records[m_position].changes) {
		change->redo();
	}
	++m_position;
	return true;
}

void edt::document_history::clear()
{
	m_records.clear();
	m_position = 0;
	m_groups.clear();
	m_saved.reset();
}

void edt::document_history::mark_saved()
{
	m_saved = top_id();
}

bool edt::document_history::is_at_save_point() const
{
	return m_saved && *m_saved == top_id();
}

std::uint64_t edt::document_history::begin_group(std::string label, const cmd::origin& origin)
{
	const std::uint64_t id = m_next_group++;
	m_groups[origin_key(origin)] = open_group_state{ id, std::move(label) };
	return id;
}

void edt::document_history::end_group(std::uint64_t group)
{
	std::erase_if(m_groups, [&](const auto& entry) { return entry.second.id == group; });
}

std::uint64_t edt::document_history::open_group(const cmd::origin& origin) const
{
	const auto it = m_groups.find(origin_key(origin));
	return it == m_groups.end() ? 0 : it->second.id;
}

edt::rollback_result edt::document_history::rollback_group(std::uint64_t group)
{
	rollback_result result;
	if (group == 0) {
		return result;
	}
	drop_redo();
	const auto shares_key = [](const history_change& a, const history_change& b) {
		return std::any_of(a.keys().begin(), a.keys().end(), [&](const std::string& key) {
			return std::find(b.keys().begin(), b.keys().end(), key) != b.keys().end();
		});
	};
	bool rolled = false;
	for (std::size_t r = m_records.size(); r-- > 0;) {
		auto& entry = m_records[r];
		if (entry.group != group) {
			continue;
		}
		while (!entry.changes.empty() && !result.conflict_key) {
			auto& change = *entry.changes.back();
			// A later record of someone else that changed the same thing: undoing this edit would lose theirs.
			for (std::size_t later = r + 1; later < m_records.size() && !result.conflict_key; ++later) {
				const auto& other = m_records[later];
				if (other.group == group || origin_key(other.origin) == origin_key(entry.origin)) {
					continue;
				}
				for (const auto& other_change : other.changes) {
					if (!shares_key(change, *other_change)) {
						continue;
					}
					const auto key = std::find_if(change.keys().begin(), change.keys().end(), [&](const std::string& k) {
						return std::find(other_change->keys().begin(), other_change->keys().end(), k) != other_change->keys().end();
					});
					result.conflict_key = *key;
					result.conflict_label = other.label;
					break;
				}
			}
			if (result.conflict_key) {
				break;
			}
			change.undo();
			entry.changes.pop_back();
			++result.rolled_back;
			rolled = true;
		}
		result.remaining += entry.changes.size();
	}
	if (rolled) {
		// What was saved is no state of the history any more.
		m_saved.reset();
		std::erase_if(m_records, [](const history_record& r) { return r.changes.empty(); });
		m_position = m_records.size();
	}
	if (!result.conflict_key) {
		end_group(group);
	}
	return result;
}
