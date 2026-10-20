#pragma once
#include "edt_registration.h"
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class editor_context;

	// What the editor does with one kind of asset.
	struct asset_kind_desc
	{
		std::string id{};     // "prefab"
		// Matches the assets whose res-relative path ends with this text, e.g. ".prefab.desc" (case-sensitive).
		// Empty: matches every asset, so it is the fallback of the kinds that have a longer suffix.
		std::string suffix{};
		// Short text the asset browser may show in front of the name.
		std::string icon{};
		// Double-click in the asset browser. Null: this kind has no open action.
		std::function<void(editor_context&, const res::tag&)> open{};
		// Drop onto the viewport. Null: this kind cannot be dropped into the scene.
		std::function<void(editor_context&, const res::tag&)> drop_into_scene{};
	};

	// Asset kinds in registration order. For a path the kind with the longest matching suffix that has the
	// wanted action (open / drop_into_scene) handles it; on a tie the earlier registration wins. Built-in kinds
	// go through the same add() as extensions. Main thread only.
	class asset_kind_registry
	{
	public:
		asset_kind_registry();
		~asset_kind_registry();
		asset_kind_registry(const asset_kind_registry&) = delete;
		asset_kind_registry& operator=(const asset_kind_registry&) = delete;

		// An empty id, a taken id or a kind with neither open nor drop_into_scene is an error: it is logged and
		// nothing is registered (the returned registration is not active).
		[[nodiscard]] registration add(asset_kind_desc desc);

		bool contains(std::string_view id) const;
		// Null if there is none. The pointer is valid until the registry changes.
		const asset_kind_desc* find(std::string_view id) const;
		std::size_t size() const { return m_entries.size(); }
		// Ids in registration order.
		std::vector<std::string> ids() const;

		// The kind that would open / accept a drop of `path` (res-relative), or null.
		const asset_kind_desc* kind_for_open(std::string_view path) const;
		const asset_kind_desc* kind_for_drop(std::string_view path) const;

		// Run the matching action. False (nothing runs) if no kind handles the asset.
		bool open(editor_context& context, const res::tag& tag) const;
		bool drop_into_scene(editor_context& context, const res::tag& tag) const;

	private:
		struct entry;
		void remove(std::uint64_t key);
		const asset_kind_desc* best_match(std::string_view path, bool for_open) const;

		std::vector<std::shared_ptr<entry>> m_entries;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
