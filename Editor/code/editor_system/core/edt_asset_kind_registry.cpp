#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <utility>

struct edt::asset_kind_registry::entry
{
	std::uint64_t key = 0;
	asset_kind_desc desc;
};

edt::asset_kind_registry::asset_kind_registry() = default;
edt::asset_kind_registry::~asset_kind_registry() = default;

edt::registration edt::asset_kind_registry::add(asset_kind_desc desc)
{
	if (desc.id.empty()) {
		egLOG_ERROR("Editor/AssetKinds", "asset kind not registered: it needs an id");
		return {};
	}
	if (!desc.open && !desc.drop_into_scene && !desc.make_preview) {
		egLOG_ERROR("Editor/AssetKinds", "asset kind '{}' not registered: it has none of open, drop_into_scene and make_preview", desc.id);
		return {};
	}
	if (contains(desc.id)) {
		egLOG_ERROR("Editor/AssetKinds", "asset kind '{}' not registered: the id is already taken", desc.id);
		return {};
	}

	auto added = std::make_shared<entry>();
	added->key = m_next_key++;
	added->desc = std::move(desc);
	const std::uint64_t key = added->key;
	m_entries.push_back(std::move(added));
	return registration(m_alive, [this, key] { remove(key); });
}

void edt::asset_kind_registry::remove(std::uint64_t key)
{
	const auto it = std::find_if(m_entries.begin(), m_entries.end(), [key](const std::shared_ptr<entry>& e) { return e->key == key; });
	if (it == m_entries.end()) return;
	// Copies: an observer may add or remove registrations.
	const std::shared_ptr<entry> removed = *it;
	const auto observers = m_removed_observers;
	for (const auto& observer : observers) observer.second(removed->desc);
	std::erase_if(m_entries, [key](const std::shared_ptr<entry>& e) { return e->key == key; });
}

edt::registration edt::asset_kind_registry::on_removed(std::function<void(const asset_kind_desc&)> observer)
{
	const std::uint64_t key = m_next_key++;
	m_removed_observers.emplace_back(key, std::move(observer));
	return registration(m_alive, [this, key] {
		std::erase_if(m_removed_observers, [key](const auto& item) { return item.first == key; });
	});
}

bool edt::asset_kind_registry::contains(std::string_view id) const
{
	return find(id) != nullptr;
}

const edt::asset_kind_desc* edt::asset_kind_registry::find(std::string_view id) const
{
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	return it != m_entries.end() ? &(*it)->desc : nullptr;
}

std::vector<std::string> edt::asset_kind_registry::ids() const
{
	std::vector<std::string> out;
	out.reserve(m_entries.size());
	for (const auto& e : m_entries) out.push_back(e->desc.id);
	return out;
}

const edt::asset_kind_desc* edt::asset_kind_registry::best_match(std::string_view path, action wanted) const
{
	const asset_kind_desc* best = nullptr;
	for (const auto& e : m_entries) {
		const asset_kind_desc& desc = e->desc;
		const bool has_action = wanted == action::open ? static_cast<bool>(desc.open)
			: wanted == action::drop ? static_cast<bool>(desc.drop_into_scene) : static_cast<bool>(desc.make_preview);
		if (!has_action) continue;
		if (!path.ends_with(desc.suffix)) continue;
		// Strictly longer: on a tie the earlier registration stays.
		if (!best || desc.suffix.size() > best->suffix.size()) best = &desc;
	}
	return best;
}

const edt::asset_kind_desc* edt::asset_kind_registry::kind_for_open(std::string_view path) const
{
	return best_match(path, action::open);
}

const edt::asset_kind_desc* edt::asset_kind_registry::kind_for_drop(std::string_view path) const
{
	return best_match(path, action::drop);
}

const edt::asset_kind_desc* edt::asset_kind_registry::kind_for_preview(std::string_view path) const
{
	return best_match(path, action::preview);
}

bool edt::asset_kind_registry::open(editor_context& context, const res::tag& tag) const
{
	const asset_kind_desc* kind = kind_for_open(tag.relative());
	if (!kind) return false;
	// A copy: the action may add or remove registrations, including its own.
	const auto run = kind->open;
	run(context, tag);
	return true;
}

bool edt::asset_kind_registry::drop_into_scene(editor_context& context, const res::tag& tag) const
{
	const asset_kind_desc* kind = kind_for_drop(tag.relative());
	if (!kind) return false;
	const auto run = kind->drop_into_scene;
	run(context, tag);
	return true;
}
