#include <snk/common/ds/ds_store.hpp>

struct ds::app_data_storage::entry {
	std::uint64_t key;
	std::string_view name;
	std::shared_ptr<void> object;
};

ds::app_data_storage::app_data_storage() = default;

ds::app_data_storage::app_data_storage(app_data_storage* parent)
	: m_parent(parent)
{
}

ds::app_data_storage::app_data_storage(app_data_storage* parent, isolated_t)
	: m_parent(parent), m_isolated(true)
{
}

ds::app_data_storage::app_data_storage(app_data_storage&& other) = default;

ds::app_data_storage& ds::app_data_storage::operator=(app_data_storage&& other)
{
	if (this != &other) {
		destroy_all();
		m_entries = std::move(other.m_entries);
		m_index = std::move(other.m_index);
		m_parent = other.m_parent;
		m_isolated = other.m_isolated;
		m_exposed = std::move(other.m_exposed);
	}
	return *this;
}

ds::app_data_storage::~app_data_storage()
{
	destroy_all();
}

std::size_t ds::app_data_storage::size() const noexcept
{
	return m_entries.size();
}

void ds::app_data_storage::ensure_absent(std::uint64_t key, std::string_view name) const
{
	auto it = m_index.find(key);
	if (it == m_index.end()) {
		return;
	}
	const entry& existing = m_entries[it->second];
	if (existing.name == name) {
		throw std::logic_error(std::string{ "ds::app_data_storage::construct: service already registered: " } + std::string{ name });
	}
	throw std::logic_error(std::string{ "ds::app_data_storage: key collision between " } + std::string{ existing.name } + " and " + std::string{ name });
}

void ds::app_data_storage::insert(std::uint64_t key, std::string_view name, std::shared_ptr<void> object)
{
	ensure_absent(key, name);
	m_index.emplace(key, m_entries.size());
	m_entries.push_back(entry{ key, name, std::move(object) });
}

void ds::app_data_storage::rebuild_index()
{
	m_index.clear();
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		m_index.emplace(m_entries[i].key, i);
	}
}

const ds::app_data_storage::entry* ds::app_data_storage::find_entry(std::uint64_t key, std::string_view name) const
{
	for (const app_data_storage* storage = this; storage != nullptr; storage = storage->m_parent) {
		auto it = storage->m_index.find(key);
		if (it != storage->m_index.end() && storage->m_entries[it->second].name == name) {
			return &storage->m_entries[it->second];
		}
		if (storage->m_isolated && !storage->m_exposed.contains(key)) {
			return nullptr;
		}
	}
	return nullptr;
}

bool ds::app_data_storage::contains(std::uint64_t key, std::string_view name) const
{
	return find_entry(key, name) != nullptr;
}

void* ds::app_data_storage::find_object(std::uint64_t key, std::string_view name) const
{
	const entry* found = find_entry(key, name);
	return found ? found->object.get() : nullptr;
}

std::shared_ptr<void> ds::app_data_storage::find_shared(std::uint64_t key, std::string_view name) const
{
	const entry* found = find_entry(key, name);
	return found ? found->object : nullptr;
}

bool ds::app_data_storage::hidden(std::uint64_t key, std::string_view name) const
{
	for (const app_data_storage* storage = this; storage != nullptr; storage = storage->m_parent) {
		if (storage->m_isolated && !storage->m_exposed.contains(key)) {
			return storage->m_parent && storage->m_parent->find_entry(key, name) != nullptr;
		}
	}
	return false;
}

void ds::app_data_storage::destroy_all()
{
	while (!m_entries.empty()) {
		m_index.erase(m_entries.back().key);
		m_entries.pop_back();
	}
}

void ds::app_data_storage::erase_key(std::uint64_t key, std::string_view name)
{
	auto it = m_index.find(key);
	if (it == m_index.end() || m_entries[it->second].name != name) {
		throw std::logic_error(std::string{ "ds::app_data_storage::destruct: service not registered: " } + std::string{ name });
	}
	// Take the entry out first, so the object is destroyed after the index is consistent again.
	entry removed = std::move(m_entries[it->second]);
	m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(it->second));
	rebuild_index();
}

void ds::app_data_storage::expose_key(std::uint64_t key)
{
	m_exposed.insert(key);
}

ds::app_data_storage* ds::app_data_storage::parent() const
{
	return m_parent;
}
