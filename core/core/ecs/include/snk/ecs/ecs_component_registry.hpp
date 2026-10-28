#pragma once
#include <snk/common/logger/engine_log.h>
#include <entt/core/fwd.hpp>
#include <entt/core/type_info.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ecs {

// Stable component names (saved data, wasm modules, the editor) mapped to the storage ids of the
// registry. A C++ component is stored under entt::type_hash<T>; a component without a C++ type
// (#16, wasm) will get its id from its name. A layer over `parent` (MODULE_LAYER.md) sees the
// parent's names and cannot reuse them or their ids.
class component_registry
{
public:
	component_registry() = default;
	explicit component_registry(const component_registry* parent)
		: m_parent(parent)
	{
	}

	template <typename T>
	bool add(std::string name)
	{
		return add(std::move(name), entt::type_hash<T>::value());
	}

	// False (logged, nothing added) when the name or the id is taken here or in the parent.
	bool add(std::string name, entt::id_type id)
	{
		if (name.empty()) {
			egLOG_ERROR("ecs", "component not registered: empty name");
			return false;
		}
		if (find(name)) {
			egLOG_ERROR("ecs", "component '{}' not registered: the name is taken", name);
			return false;
		}
		if (const std::string* other = name_of(id)) {
			egLOG_ERROR("ecs", "component '{}' not registered: its id is taken by '{}'", name, *other);
			return false;
		}
		m_names.emplace(id, name);
		m_ids.emplace(std::move(name), id);
		return true;
	}

	// Removes one of this layer's names; false for an unknown name or one of the parent's.
	bool remove(std::string_view name)
	{
		auto it = m_ids.find(std::string{ name });
		if (it == m_ids.end()) {
			return false;
		}
		m_names.erase(it->second);
		m_ids.erase(it);
		return true;
	}

	[[nodiscard]] std::optional<entt::id_type> find(std::string_view name) const
	{
		if (auto it = m_ids.find(std::string{ name }); it != m_ids.end()) {
			return it->second;
		}
		return m_parent ? m_parent->find(name) : std::nullopt;
	}

	[[nodiscard]] const std::string* name_of(entt::id_type id) const
	{
		if (auto it = m_names.find(id); it != m_names.end()) {
			return &it->second;
		}
		return m_parent ? m_parent->name_of(id) : nullptr;
	}

	// This layer's own names, without the parent's.
	[[nodiscard]] std::size_t size() const noexcept { return m_ids.size(); }

private:
	const component_registry* m_parent = nullptr;
	std::unordered_map<std::string, entt::id_type> m_ids;
	std::unordered_map<entt::id_type, std::string> m_names;
};

} // namespace ecs
