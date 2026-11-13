#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class job_service;

	// One file of the resource tree as the asset database knows it (EPIC-57 section 4.3).
	struct asset_record
	{
		res::tag path;
		// Index into the layers (lowest first) of the layer whose file wins for the path.
		std::size_t layer = 0;
		// Lower layers that hold an entry of the same name, shadowed by this one (Problems warns about an engine asset
		// a project file covers, R28).
		std::vector<std::size_t> shadowed_layers;
		// From the file's .resource; none: the file has no ID yet ("no ID", S57-4).
		std::optional<res::asset_id> id;
		std::int64_t size = 0;
		std::int64_t mtime = 0; // ticks of std::filesystem::file_time_type
		bool has_meta = false;
		std::int64_t meta_size = 0;
		std::int64_t meta_mtime = 0;
		// Why the .resource next to the file could not be read (with the path of the bad field); empty when it was read.
		std::string meta_error;
		std::vector<std::string> flags;
		// The kind of asset by suffix: desc, texture, model, shader or file.
		std::string type;
		// "fnv1a64:<16 hex digits>"; empty until the background pass hashed the file.
		std::string content_hash;
		// The "__type" of a desc file's root, once hashed; none for any other file (or a desc without one).
		std::optional<std::string> desc_type;
	};

	// A .resource without its file.
	struct asset_orphan
	{
		res::tag meta_path;
		std::optional<res::asset_id> id;
	};

	// What a walk of the layers found, built off the main thread.
	struct asset_scan
	{
		std::vector<asset_record> records;
		std::vector<asset_orphan> orphans;
	};

	// What the background pass reads of one file: its hash and, for a desc, its "__type".
	struct asset_content
	{
		bool ok = false;
		std::string error;
		// The file's size and time before and after the read: equal when nothing wrote it meanwhile.
		std::int64_t size = 0;
		std::int64_t mtime = 0;
		bool unchanged = false;
		std::string content_hash;
		std::optional<std::string> desc_type;
	};

	inline constexpr std::string_view k_asset_meta_suffix = ".resource";

	// The kind of asset of a path by its suffix (lowercase compare).
	std::string asset_type_of(std::string_view path);

	// Walks `layers` (lowest first, as resource_system::get_resource_layers gives them) merged as the asset browser merges
	// them (an upper layer shadows a name below, case folded on Windows), and reads every file's .resource. In every
	// layer, an entry that redirects (a link, a junction) is neither listed nor entered, and hidden folders are skipped.
	// `stopped` (may be empty) ends the walk early.
	asset_scan scan_asset_layers(const std::vector<std::filesystem::path>& layers, const std::function<bool()>& stopped = {});

	// Hashes the file at `relative` below `layer_root` through a read that follows no link (platform::beneath), in chunks;
	// a desc file of at most `max_desc_bytes` is parsed for its "__type" too.
	asset_content read_asset_content(const std::filesystem::path& layer_root, const std::filesystem::path& relative,
		bool is_desc, std::uint64_t max_desc_bytes);

	// The editor's asset database (EPIC-57 section 4.3, S57-3): ID <-> path, size and time, content hash, kind and desc
	// type of every file of res://, built from the files' .resource. The walk and the hashing run on job_service (lane
	// io); their results are applied on the main thread. Main thread only.
	class asset_database
	{
	public:
		static constexpr std::uint64_t max_desc_bytes = 4u * 1024u * 1024u;
		static constexpr std::size_t hash_batch = 64;
		static constexpr int max_depth = 64;

		// `events` (may be null) tells the database about a job someone cancelled (job.cancel): its work is queued again
		// and goes on at the next rebuild. Without it a cancelled hashing job is only noticed by rebuild().
		asset_database(job_service& jobs, editor_events* events, std::vector<std::filesystem::path> layers);
		~asset_database();
		asset_database(const asset_database&) = delete;
		asset_database& operator=(const asset_database&) = delete;

		// Walks the layers again; what is known stays until the walk's result arrives. A walk already running is cancelled.
		void rebuild();
		// A walk is running.
		bool scanning() const { return m_scanning; }
		// Files whose hash is not known yet (queued or running).
		std::size_t pending_hashes() const;

		const asset_record* find(const res::tag& path) const;
		std::vector<const asset_record*> find(const res::asset_id& id) const;
		const std::map<std::string, asset_record>& records() const { return m_records; }
		// IDs that more than one file carries.
		std::vector<res::asset_id> duplicates() const;
		const std::vector<asset_orphan>& orphans() const { return m_orphans; }
		// Grows whenever anything the database holds changed.
		std::uint64_t revision() const { return m_revision; }

	private:
		void apply_scan(asset_scan scan);
		void hash_next();
		void index_id(const asset_record& record);
		void on_job_finished(std::uint64_t id, job_status status);
		// The keys of the hashing job that ended without an answer for them go back into the queue.
		void requeue_in_flight();

		job_service& m_jobs;
		std::vector<std::filesystem::path> m_layers;
		std::map<std::string, asset_record> m_records; // by path text
		std::multimap<res::asset_id, std::string> m_by_id;
		std::vector<asset_orphan> m_orphans;
		std::vector<std::string> m_unhashed;
		std::vector<std::string> m_in_flight;
		std::uint64_t m_hash_job = 0;
		std::uint64_t m_scan_job = 0;
		bool m_scanning = false;
		std::uint64_t m_scan_number = 0;
		std::uint64_t m_revision = 0;
		connection m_job_finished;
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}
