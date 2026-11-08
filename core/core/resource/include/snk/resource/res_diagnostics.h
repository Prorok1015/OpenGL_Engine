#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/common/ds/ds_type_id.hpp>

namespace core::res { struct res_control_block_base; }

// What went wrong in resource loads, as "resource + JSON path + reason" (#595). A load records its problems on its
// own control block; diagnostics_collector gathers them over everything a request loaded.
namespace res
{
	enum class diagnostic_severity : std::uint8_t { error, warning };

	namespace diagnostic_kind
	{
		inline constexpr std::string_view unknown_type = "unknown_type";
		inline constexpr std::string_view missing_parent = "missing_parent";
		inline constexpr std::string_view cycle = "cycle";
		inline constexpr std::string_view schema = "schema";
		inline constexpr std::string_view unknown_component = "unknown_component";
		// A desc of one type where another is expected (a material file as a level, #615). The load itself succeeded.
		inline constexpr std::string_view type_mismatch = "type_mismatch";
		// Any other failure of a load (no file, no adapter, a parse error, ...).
		inline constexpr std::string_view load = "load";
		// The collector's own entry when it kept no more: "N more diagnostics".
		inline constexpr std::string_view truncated = "truncated";
	}

	struct diagnostic
	{
		diagnostic_severity severity = diagnostic_severity::error;
		std::string kind;
		// The file the problem is in: for a desc written inline in another file, that file.
		std::string resource;
		// A JSON pointer into `resource`; empty: the whole file.
		std::string json_path;
		std::string reason;

		bool operator==(const diagnostic&) const = default;
	};

	// One JSON pointer token: "~" -> "~0", "/" -> "~1".
	SNK_API std::string json_pointer_token(std::string_view key);

	// Thrown by an adapter: the load fails, and the block records `diagnostic` (resource left empty: the block's own).
	class SNK_API load_error : public std::runtime_error
	{
	public:
		load_error(std::string_view kind, std::string_view json_path, const std::string& reason);
		const diagnostic& what_diagnostic() const { return m_diagnostic; }

	private:
		diagnostic m_diagnostic;
	};

	// A load another one started. `expected` (id 0: any) is the type the field asked for, checked by the collector.
	struct child_link
	{
		std::weak_ptr<core::res::res_control_block_base> block;
		std::string json_path;
		bool inline_copy = false;
		ds::type_id expected;
		std::string expected_name;
	};

	// The problems one load recorded on its control block, and what the collector needs to walk from it.
	class SNK_API load_diagnostics
	{
	public:
		// Kept per block; more are only counted (a big broken file cannot fill memory).
		static constexpr std::size_t max_items = 64;

		struct snapshot
		{
			std::string resource;
			std::vector<diagnostic> items;
			std::size_t dropped = 0;
			std::weak_ptr<core::res::res_control_block_base> origin;
			std::string origin_path;
			std::vector<child_link> children;
		};

		void set_resource(std::string resource);
		// `item.resource` empty: this block's resource.
		void add(diagnostic item);
		// A load this one started: its dependencies and the descs of its fields.
		void add_child(const std::shared_ptr<core::res::res_control_block_base>& child, child_link link = {});
		// This block is the memory:// copy of a desc written inline at `json_path` of `owner`: its problems are reported
		// there. The cache key stays the copy's. An owner still alive keeps its claim; a dead one is replaced. Owners
		// whose files have the same name and write the same inline desc share one copy (desc_system names it by the
		// owner's name and the content), so the second one's request reports the problem at the first one's file.
		void set_origin(const std::shared_ptr<core::res::res_control_block_base>& owner, std::string json_path);

		snapshot read() const;

	private:
		mutable std::mutex m_mutex;
		std::string m_resource;
		std::vector<diagnostic> m_items;
		std::size_t m_dropped = 0;
		std::weak_ptr<core::res::res_control_block_base> m_origin;
		std::string m_origin_path;
		std::vector<child_link> m_children;
	};

	// While an adapter deserializes `block` on this thread (resource_system sets it): report_load_warning and
	// note_field_load go to that block. Scopes nest (a load run inside another one on the same thread).
	class SNK_API load_scope
	{
	public:
		explicit load_scope(std::shared_ptr<core::res::res_control_block_base> block);
		~load_scope();
		load_scope(const load_scope&) = delete;
		load_scope& operator=(const load_scope&) = delete;

		static load_scope* current();
		const std::shared_ptr<core::res::res_control_block_base>& block() const { return m_block; }

	private:
		std::shared_ptr<core::res::res_control_block_base> m_block;
		load_scope* m_previous = nullptr;
	};

	// Adds `item` while `block` has not settled, under the lock that settles it: whoever sees the block settled sees
	// every diagnostic it will ever have. False (nothing added) once it has settled.
	SNK_API bool add_while_unsettled(core::res::res_control_block_base& block, diagnostic item);

	// A problem that does not fail the load, of the resource deserialized on this thread. Outside a load: nothing.
	SNK_API void report_load_warning(std::string_view kind, std::string_view json_path, std::string reason);

	// A load started for a field at `json_path` of the resource deserialized on this thread: the collector walks into
	// it, and when `inline_copy` (a memory:// copy of an inline desc) its problems are reported at the owner's file and
	// path. The collector reports a `type_mismatch` there when the field's desc is not of `expected` (id 0: not checked).
	// Outside a load: nothing.
	SNK_API void note_field_load(const std::shared_ptr<core::res::res_control_block_base>& child, std::string_view json_path,
		bool inline_copy, ds::type_id expected = {}, std::string_view expected_name = {});

	// The diagnostics of settled requests and of everything they loaded (dependencies, field descs), each block once.
	// A cached block gives the same diagnostics to every request. At most `limit` are kept; the rest show as one
	// "N more diagnostics" entry. Thread safe; collect() walks without holding the collector's lock, so items() never
	// waits for a walk.
	class SNK_API diagnostics_collector
	{
	public:
		static constexpr std::size_t default_limit = 200;

		explicit diagnostics_collector(std::size_t limit = default_limit);

		void collect(const std::shared_ptr<core::res::res_control_block_base>& block);
		void add(diagnostic item);

		// What was kept, then a `truncated` warning when more were dropped.
		std::vector<diagnostic> items() const;
		std::size_t error_count() const;

	private:
		void add_locked(diagnostic item);

		mutable std::mutex m_mutex;
		std::size_t m_limit;
		std::vector<diagnostic> m_items;
		std::size_t m_dropped = 0;
		std::unordered_set<const void*> m_seen;
		// Keeps the walked blocks alive, so a freed block's address is never taken for one already seen.
		std::vector<std::shared_ptr<core::res::res_control_block_base>> m_walked;
	};
}
