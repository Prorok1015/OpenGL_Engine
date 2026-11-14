#include "edt_asset_index.h"
#include "edt_asset_database.h"
#include "edt_asset_layers.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>
#include <string_view>
#include <unordered_set>
#include <vector>

std::optional<res::asset_id> edt::asset_index_snapshot::id_of(const res::tag& tag) const
{
	const auto snapshot = current();
	const auto found = snapshot->ids.find(key_of(tag, snapshot->case_insensitive));
	return found == snapshot->ids.end() ? std::nullopt : std::optional{ found->second };
}

std::optional<res::tag> edt::asset_index_snapshot::path_of(const res::asset_id& id) const
{
	const auto snapshot = current();
	const auto found = snapshot->paths.find(id);
	return found == snapshot->paths.end() ? std::nullopt : std::optional{ found->second };
}

std::shared_ptr<const edt::asset_index_snapshot::data> edt::asset_index_snapshot::current() const
{
	std::lock_guard lock(m_mutex);
	return m_data;
}

std::string edt::asset_index_snapshot::key_of(const res::tag& tag, bool case_insensitive)
{
	return layer_merge_key(std::string{ tag.view() }, case_insensitive);
}

void edt::asset_index_snapshot::warn_copy(const asset_record& copy, const asset_record& holder)
{
	if (m_warned.emplace(*copy.id, std::string{ copy.path.view() }).second) {
		egLOG_WARN("assets", "'{}' carries the ID {} of '{}': it loads as a file of its own until it gets an ID of its own "
			"(Problems)", copy.path.view(), res::to_text(*copy.id), holder.path.view());
	}
}

bool edt::asset_index_snapshot::refresh(const asset_database& assets)
{
	++m_full_refreshes;
	auto next = std::make_shared<data>();
	next->case_insensitive = assets.case_insensitive();
	std::unordered_map<res::asset_id, const asset_record*> holders;
	for (const auto& [key, record] : assets.records()) {
		if (!record.id || record.id->is_nil()) {
			continue;
		}
		const auto [held, inserted] = holders.try_emplace(*record.id, &record);
		if (!inserted && holds_id_before(record, *held->second)) {
			held->second = &record;
		}
	}
	for (const auto& [key, record] : assets.records()) {
		if (!record.id || record.id->is_nil()) {
			continue;
		}
		const asset_record& holder = *holders.at(*record.id);
		if (&holder != &record) {
			warn_copy(record, holder);
		}
	}
	for (const auto& [id, holder] : holders) {
		next->ids.emplace(key_of(holder->path, next->case_insensitive), id);
		next->paths.emplace(id, holder->path);
	}

	std::lock_guard lock(m_mutex);
	if (*next == *m_data) {
		return false;
	}
	m_data = std::move(next);
	return true;
}

bool edt::asset_index_snapshot::apply_batch(const asset_database& assets, const asset_batch& batch)
{
	const auto before = current();
	if (before->case_insensitive != assets.case_insensitive()) {
		return refresh(assets);
	}
	std::vector<const res::tag*> paths;
	paths.reserve(batch.moved.size() * 2 + batch.added.size() + batch.removed.size() + batch.changed.size());
	for (const auto& [from, to] : batch.moved) {
		paths.push_back(&from);
		paths.push_back(&to);
	}
	for (const auto* list : { &batch.added, &batch.removed, &batch.changed }) {
		for (const res::tag& path : *list) {
			paths.push_back(&path);
		}
	}
	// A path that held no ID here holds none of the answers: its old ID, if any, keeps its holder.
	std::unordered_set<res::asset_id> touched;
	for (const res::tag* path : paths) {
		if (const auto held = before->ids.find(key_of(*path, before->case_insensitive)); held != before->ids.end()) {
			touched.insert(held->second);
		}
		if (const asset_record* record = assets.find(*path); record && record->id && !record->id->is_nil()) {
			touched.insert(*record->id);
		}
	}

	std::vector<std::pair<res::asset_id, const asset_record*>> holders;
	holders.reserve(touched.size());
	bool same = true;
	for (const res::asset_id& id : touched) {
		const std::vector<const asset_record*> carriers = assets.find(id);
		const asset_record* holder = nullptr;
		for (const asset_record* record : carriers) {
			if (!holder || holds_id_before(*record, *holder)) {
				holder = record;
			}
		}
		for (const asset_record* record : carriers) {
			if (record != holder) {
				warn_copy(*record, *holder);
			}
		}
		const auto answered = before->paths.find(id);
		const bool had = answered != before->paths.end();
		same = same && (holder ? had && answered->second == holder->path : !had);
		holders.emplace_back(id, holder);
	}
	if (same) {
		return false;
	}

	auto next = std::make_shared<data>(*before);
	// Every old answer goes before any new one is put in: two IDs that swap paths keep both.
	for (const auto& [id, holder] : holders) {
		if (const auto old = next->paths.find(id); old != next->paths.end()) {
			const auto key = next->ids.find(key_of(old->second, next->case_insensitive));
			if (key != next->ids.end() && key->second == id) {
				next->ids.erase(key);
			}
			next->paths.erase(old);
		}
	}
	for (const auto& [id, holder] : holders) {
		if (holder) {
			next->ids.insert_or_assign(key_of(holder->path, next->case_insensitive), id);
			next->paths.emplace(id, holder->path);
		}
	}
	std::lock_guard lock(m_mutex);
	m_data = std::move(next);
	return true;
}

edt::asset_index_link::asset_index_link(asset_database& assets, res::resource_system& resources)
	: m_assets(assets)
	, m_resources(resources)
{
	m_assets.set_on_batch([this](const asset_batch& batch) { on_batch(batch); });
}

edt::asset_index_link::~asset_index_link()
{
	m_assets.set_on_batch({});
	if (m_attached) {
		m_resources.set_asset_index(nullptr);
	}
}

void edt::asset_index_link::attach()
{
	m_index->refresh(m_assets);
	m_id_revision = m_assets.id_revision();
	m_drops_seen = m_assets.changes_dropped();
	m_resources.set_asset_index(m_index);
	m_attached = true;
}

void edt::asset_index_link::detach()
{
	// What waits for a batch is evicted now (set_asset_index), entries move back to their tags.
	m_resources.set_asset_index(nullptr);
	m_attached = false;
	m_moves.clear();
	m_attach_after = m_assets.walks_applied();
	m_drops_seen = m_assets.changes_dropped();
}

bool edt::asset_index_link::follow_drops()
{
	if (m_attached && m_assets.changes_dropped() != m_drops_seen) {
		detach();
	}
	if (!m_attached) {
		// Dropped again while detached: the walk that would have caught up with the first drop is behind the second.
		if (m_assets.changes_dropped() != m_drops_seen) {
			m_drops_seen = m_assets.changes_dropped();
			m_attach_after = m_assets.walks_applied();
		}
		if (m_assets.walks_applied() > m_attach_after && !m_assets.scanning()) {
			attach();
		}
	}
	return m_attached;
}

void edt::asset_index_link::apply(std::span<const std::pair<res::tag, res::tag>> moved)
{
	const std::uint64_t through = m_assets.applied_through();
	std::vector<std::pair<res::tag, res::tag>> all(moved.begin(), moved.end());
	for (const handed_move& earlier : m_moves) {
		all.push_back(earlier.move);
	}
	m_resources.apply_asset_batch(through, all);
	// A batch with a higher number than an earlier move's has decided its file (or it no longer waits).
	std::erase_if(m_moves, [through](const handed_move& earlier) { return earlier.through < through; });
	for (const auto& move : moved) {
		m_moves.push_back(handed_move{ move, through });
	}
}

void edt::asset_index_link::poll()
{
	if (!follow_drops()) {
		return;
	}
	// Every step comes with a batch (id_revision); one a batch did not take is caught here.
	if (m_assets.id_revision() != m_id_revision) {
		m_id_revision = m_assets.id_revision();
		if (m_index->refresh(m_assets)) {
			apply({});
		}
	}
}

void edt::asset_index_link::on_batch(const asset_batch& batch)
{
	const bool was_attached = m_attached;
	if (!follow_drops() || !was_attached) {
		// Attached just now (or not yet): nothing waited for this batch, the index starts from what the database holds.
		return;
	}
	// A walk's batch can name every file: the whole snapshot is taken again. A watcher's touches a few IDs.
	if (batch.from_walk) {
		m_index->refresh(m_assets);
		m_id_revision = m_assets.id_revision();
	} else {
		m_index->apply_batch(m_assets, batch);
		// Only this batch's step is taken: a change that came without a batch since the last poll is left to it.
		if (m_assets.id_revision() == m_id_revision + 1) {
			m_id_revision = m_assets.id_revision();
		}
	}
	apply(batch.moved);
}
