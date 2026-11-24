#pragma once
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class asset_kind_registry;

	// The kinds the asset browser filters by: one kind per file, from its suffix, the `__type` of a desc, the registry's kinds
	// (a module's) and asset_type_of, in this order of asking. Folders have no kind and always pass the filter (#1191).
	inline constexpr std::string_view k_asset_kind_level = "level";
	inline constexpr std::string_view k_asset_kind_world = "world";
	inline constexpr std::string_view k_asset_kind_mesh = "mesh";
	inline constexpr std::string_view k_asset_kind_prefab = "prefab";
	inline constexpr std::string_view k_asset_kind_shader_desc = "shader_desc";
	inline constexpr std::string_view k_asset_kind_data = "data";

	struct asset_filter_kind
	{
		std::string id;
		std::string label;
	};

	// The kind of the file `path` (res-relative or a name). `desc_type` is the `__type` of a .desc file when it is known
	// (`sniff_desc_type`), empty otherwise; it tells a level from other data where the suffix does not. `kinds` may be null.
	std::string asset_kind_of(const asset_kind_registry* kinds, std::string_view path, std::string_view desc_type = {});

	// The `__type` value near the start of a .desc file (the first 4 KiB), empty when there is none or the file cannot be read.
	std::string sniff_desc_type(const std::filesystem::path& file);

	// What the "Kinds" list offers: the fixed kinds, then the registry's kinds that are none of them.
	std::vector<asset_filter_kind> asset_filter_kinds(const asset_kind_registry* kinds);

	// The browser's filter: a part of the name (no case) and a set of kinds, which both have to hold. No kind picked = all.
	class asset_filter
	{
	public:
		void set_name(std::string_view text);
		void set_kind(std::string_view id, bool on);
		void clear_kinds() { m_kinds.clear(); }
		bool kind_picked(std::string_view id) const { return m_kinds.contains(std::string{ id }); }
		const std::set<std::string, std::less<>>& kinds() const { return m_kinds; }

		bool needs_kind() const { return !m_kinds.empty(); }
		bool active() const { return !m_name.empty() || !m_kinds.empty(); }

		// A folder passes always; a file when its name has the text and, with kinds picked, its kind is one of them.
		bool accepts(bool is_dir, std::string_view name, std::string_view kind) const;

		// "All", the kind's id when one is picked, else "N kinds".
		std::string summary() const;

	private:
		std::string m_name;
		std::set<std::string, std::less<>> m_kinds;
	};
}
