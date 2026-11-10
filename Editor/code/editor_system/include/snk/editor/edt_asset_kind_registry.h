#pragma once
#include "edt_api.h"
#include "edt_document.h"
#include "edt_registration.h"
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	class editor_context;
	struct editor_services;

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

	// What previews the assets of a suffix (editor API 1.1, #589): the document the document_manager opens for them, e.g.
	// the prefab editor's. Registered apart from the asset kinds: asset_kind_desc crosses the module boundary by value
	// (editor_module_context::add_asset_kind), so a field added to it would change the layout a 1.0 module was built with.
	struct asset_preview_desc
	{
		std::string id{};     // "prefab"; unique among the previews
		// The previewed assets are the ones whose res-relative path ends with this text (case-sensitive); the longest
		// matching suffix makes the preview. Empty: every asset.
		std::string suffix{};
		// Makes the document; called the first time a preview of this id opens, the document is kept until the preview
		// is removed. Required.
		std::function<std::unique_ptr<previewable_document>(editor_services&)> make_preview{};
	};

	// Asset kinds in registration order. For a path the kind with the longest matching suffix that has the
	// wanted action (open / drop_into_scene) handles it; on a tie the earlier registration wins. Built-in kinds
	// go through the same add() as extensions. Main thread only.
	class SNK_EDITOR_API asset_kind_registry
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

		// An empty id, a taken id or a null make_preview is an error: it is logged and nothing is registered (the returned
		// registration is not active).
		[[nodiscard]] registration add_preview(asset_preview_desc desc);
		// Null if there is none. The pointer is valid until the registry changes.
		const asset_preview_desc* find_preview(std::string_view id) const;
		// The preview with the longest suffix that matches `path` (res-relative), or null; on a tie the earlier
		// registration wins.
		const asset_preview_desc* preview_for(std::string_view path) const;
		// Called with a preview just before it is removed (its registration was released), while its make_preview is
		// still there, so what it made can be dropped first. Dies with the returned registration, or with the registry.
		[[nodiscard]] registration on_preview_removed(std::function<void(const asset_preview_desc&)> observer);

		// Run the matching action. False (nothing runs) if no kind handles the asset.
		bool open(editor_context& context, const res::tag& tag) const;
		bool drop_into_scene(editor_context& context, const res::tag& tag) const;

	private:
		struct entry;
		struct preview_entry;
		void remove(std::uint64_t key);
		void remove_preview(std::uint64_t key);
		const asset_kind_desc* best_match(std::string_view path, bool for_open) const;

		std::vector<std::shared_ptr<entry>> m_entries;
		std::vector<std::shared_ptr<preview_entry>> m_preview_entries;
		std::vector<std::pair<std::uint64_t, std::function<void(const asset_preview_desc&)>>> m_preview_removed_observers;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
