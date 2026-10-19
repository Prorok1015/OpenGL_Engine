#pragma once
#include "edt_registration.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	// Draws the fields of one component and returns true if it changed them.
	using inspector_renderer = std::function<bool(scn::prefab_desc::prefab_comp_node&)>;

	// Custom inspector renderers by desc `__type`. A type without one is drawn by the inspector's generic JSON
	// renderer. Built-in renderers go through the same add() as extensions. Main thread only.
	class inspector_registry
	{
	public:
		inspector_registry();
		~inspector_registry();
		inspector_registry(const inspector_registry&) = delete;
		inspector_registry& operator=(const inspector_registry&) = delete;

		// An empty type, a null renderer or a type that already has a renderer is an error: it is logged and
		// nothing is registered (the returned registration is not active).
		[[nodiscard]] registration add(std::string type, inspector_renderer renderer);
		// The type is T::__type.
		template<typename T>
		[[nodiscard]] registration add(inspector_renderer renderer)
		{
			return add(std::string(T::__type), std::move(renderer));
		}

		// Drops the renderer of `type` whoever registered it; its registration turns into a no-op. False if there was none.
		bool remove(std::string_view type);

		bool contains(std::string_view type) const;
		// Null if there is none. The pointer is valid until the registry changes.
		const inspector_renderer* find(std::string_view type) const;
		std::size_t size() const { return m_entries.size(); }
		// Registered types in registration order.
		std::vector<std::string> types() const;

		// Runs the renderer of `type`: its result (true = the data changed), or nullopt if there is none
		// and the caller falls back to the generic renderer.
		std::optional<bool> invoke(std::string_view type, scn::prefab_desc::prefab_comp_node& comp) const;

	private:
		struct entry;
		void remove_key(std::uint64_t key);

		std::vector<std::shared_ptr<entry>> m_entries;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
