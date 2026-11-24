#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
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

	// The `__type` value in the start of a .desc file's text (what `desc_type_cache` reads, at most 4 KiB); empty when there is none.
	std::string desc_type_in(std::string_view head);

	// Whether the kind of `path` depends on the `__type` of its desc: a .desc no suffix and no registry kind decides.
	bool asset_kind_needs_desc_type(const asset_kind_registry* kinds, std::string_view path);

	inline constexpr std::size_t k_desc_sniff_bytes = 4096;

	// The `__type` of .desc files, read at most 4 KiB each, once: a file is read again only when its size, write time or
	// identity changed. Reads go below the layer's root without following a link (platform::beneath), and the caller
	// gives a budget of reads per pass, so a big folder fills in over several frames instead of stalling one.
	class desc_type_cache
	{
	public:
		struct stamp
		{
			std::uint64_t size = 0;
			std::int64_t write_time_ns = 0;
			std::uint64_t file_id = 0;
			bool operator==(const stamp&) const = default;
		};

		struct ports
		{
			// The stamp of root/relative as it is (a link, a missing file, anything but a regular file: nullopt); reads nothing.
			std::function<std::optional<stamp>(const std::filesystem::path& root, const std::filesystem::path& relative)> stat;
			// The first bytes of the file, at most k_desc_sniff_bytes.
			std::function<std::string(const std::filesystem::path& root, const std::filesystem::path& relative)> read_head;
		};

		desc_type_cache();
		explicit desc_type_cache(ports from) : m_ports(std::move(from)) {}

		// The type from the cache while the stamp is the same; else read (one of `budget`). nullopt: not known yet, the
		// budget is used up. A file that is no regular file has the type "" without a read.
		std::optional<std::string> type_of(const std::filesystem::path& root, const std::filesystem::path& relative, int& budget);

		std::size_t reads() const { return m_reads; }
		void clear() { m_entries.clear(); }

	private:
		struct entry
		{
			stamp seen;
			std::string type;
		};

		ports m_ports;
		std::map<std::string, entry> m_entries;
		std::size_t m_reads = 0;
	};

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
