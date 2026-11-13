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
		if (!inserted) {
			const asset_record& other = *held->second;
			if (record.mtime < other.mtime || (record.mtime == other.mtime && record.path.view() < other.path.view())) {
				held->second = &record;
			}
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
	m_resources.set_asset_index(m_index);
	m_attached = true;
}

void edt::asset_index_link::poll()
{
	if (!m_attached) {
		if (m_assets.walked_once() && !m_assets.scanning()) {
			attach();
		}
		return;
	}
	// A walk that changed only orphans, a fix of a .resource: the records may change without a batch's moves.
	if (m_assets.revision() != m_revision) {
		m_revision = m_assets.revision();
		if (m_index->refresh(m_assets)) {
			m_resources.apply_asset_batch(m_assets.applied_through(), {});
		}
	}
}

void edt::asset_index_link::on_batch(const asset_batch& batch)
{
	if (!m_attached) {
		// No index yet, so nothing waits for this batch: the index starts from what the database holds now.
		attach();
		return;
	}
	m_index->refresh(m_assets);
	m_revision = m_assets.revision();
	m_resources.apply_asset_batch(m_assets.applied_through(), batch.moved);
}
