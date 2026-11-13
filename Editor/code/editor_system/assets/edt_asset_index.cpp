#include "edt_asset_index.h"
#include "edt_asset_database.h"
#include "edt_asset_layers.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_system.h>
#include <string_view>

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

bool edt::asset_index_snapshot::refresh(const asset_database& assets)
{
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
		if (&holder != &record && m_warned.emplace(*record.id, std::string{ record.path.view() }).second) {
			egLOG_WARN("assets", "'{}' carries the ID {} of '{}': it loads as a file of its own until it gets an ID of its own "
				"(Problems)", record.path.view(), res::to_text(*record.id), holder.path.view());
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
	m_revision = m_assets.revision();
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
}

bool edt::asset_index_link::follow_drops()
{
	if (m_attached && m_assets.changes_dropped() != m_drops_seen) {
		detach();
	}
	if (!m_attached && m_assets.walks_applied() > m_attach_after && !m_assets.scanning()) {
		attach();
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
	// A walk that changed only orphans, a fix of a .resource: the records may change without a batch's moves.
	if (m_assets.revision() != m_revision) {
		m_revision = m_assets.revision();
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
	m_index->refresh(m_assets);
	m_revision = m_assets.revision();
	apply(batch.moved);
}
