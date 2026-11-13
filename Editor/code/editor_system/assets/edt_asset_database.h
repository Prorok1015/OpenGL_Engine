#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_tag.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <snk/platform/plt_file_lock.h>

namespace res
{
	class resource_system;
}

namespace edt
{
	class job_service;
	// Orders the writes of the cache file (asset_database.cpp).
	struct asset_cache_writer;

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
		// When the file's stamp (size, mtime) and the .resource's were taken, in ticks of
		// std::filesystem::file_time_type::clock: a stamp this close to its check may not have moved for a change made
		// just after (one tick of a coarse file system), so the cache trusts what was learned from it only after it was
		// read again. Kept apart, so a .resource carried over from an older check leaves the file's own check alone.
		std::int64_t content_checked_at = 0;
		std::int64_t meta_checked_at = 0;

		// Everything but the times of the checks: what the record says of the file.
		bool same_as(const asset_record& other) const;
	};

	// A .resource without its file.
	struct asset_orphan
	{
		res::tag meta_path;
		std::optional<res::asset_id> id;

		bool operator==(const asset_orphan&) const = default;
	};

	// Where the database keeps its cache: <root>/<relative>, every folder below the root and the files reached without
	// following a link (prj::write_project_file, prj::read_project_file), so a project cannot point it outside itself.
	struct asset_cache_location
	{
		std::filesystem::path root;
		// A chain of plain names, e.g. ".cache/asset_db".
		std::filesystem::path relative;
	};

	// What a walk of the layers found, built off the main thread.
	struct asset_scan
	{
		std::vector<asset_record> records;
		std::vector<asset_orphan> orphans;
		// .resource files the walk read (an unchanged one known from the cache is not read).
		std::size_t meta_reads = 0;
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

	// What one batch of file changes did to the database.
	struct asset_batch
	{
		// (from, to): a file that went away and one that came with the guid of its .resource.
		std::vector<std::pair<res::tag, res::tag>> moved;
		std::vector<res::tag> added;
		std::vector<res::tag> removed;
		std::vector<res::tag> changed;
	};

	inline constexpr std::string_view k_asset_meta_suffix = ".resource";

	// The kind of asset of a path by its suffix (lowercase compare).
	std::string asset_type_of(std::string_view path);

	// Walks `layers` (lowest first, as resource_system::get_resource_layers gives them) merged as the asset browser merges
	// them (an upper layer shadows a name below, case folded on Windows), and reads every file's .resource. In every
	// layer, an entry that redirects (a link, a junction) is neither listed nor entered, and hidden folders are skipped.
	// `stopped` (may be empty) ends the walk early.
	// `known` (may be null): records by path text, from the cache or the last walk; a file whose .resource has the size
	// and time recorded there keeps the record's ID and flags without the .resource being read again.
	asset_scan scan_asset_layers(const std::vector<std::filesystem::path>& layers, const std::function<bool()>& stopped = {},
		const std::map<std::string, asset_record>* known = nullptr);

	// Writes the cache file of `records` (the format load_cache reads) through prj::write_project_file. The error, nullopt on
	// success.
	std::optional<std::string> write_cache(const asset_cache_location& cache, const std::vector<std::filesystem::path>& layers,
		const std::map<std::string, asset_record>& records);

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
		// A larger cache is not read (the walk then reads every file).
		static constexpr std::uint64_t max_cache_bytes = 16u * 1024u * 1024u;
		// A cache file stamp within this of the time it was checked is not trusted (FAT and SMB keep 2 s).
		static constexpr std::chrono::seconds stamp_granularity{ 2 };

		// `events` (may be null) tells the database about a job someone cancelled (job.cancel): its work is queued again
		// and goes on at the next rebuild. Without it a cancelled hashing job is only noticed by rebuild().
		// `cache` (cache://asset_db/; no root: no cache): the walk starts from assets.json there, so it reads only what
		// changed; the records of the cache are not published before the first walk confirmed them. The cache is written
		// back off the main thread when something changed and the work is done, and once more when the database goes.
		// Only the holder of the lock file next to it writes: a second editor on the project reads the cache and keeps
		// its database in memory.
		asset_database(job_service& jobs, editor_events* events, std::vector<std::filesystem::path> layers,
			asset_cache_location cache = {});
		~asset_database();
		asset_database(const asset_database&) = delete;
		asset_database& operator=(const asset_database&) = delete;

		// Walks the layers again; what is known stays until the walk's result arrives. A walk already running is cancelled.
		void rebuild();
		// A walk is running.
		bool scanning() const { return m_scanning; }
		// Files whose hash is not known yet (queued or running).
		std::size_t pending_hashes() const;
		// This database holds the lock of the cache and writes it.
		bool writes_cache() const { return m_lock != nullptr; }
		// The cache gave the records before the first walk.
		bool loaded_from_cache() const { return m_loaded_from_cache; }
		// Files read so far: .resource files and files hashed. For tests of the cache.
		std::size_t files_read() const { return m_files_read; }
		// Writes the cache now, on the calling thread, when something changed since it was last written (and the lock is
		// held).
		void save();

		// Follows the changes below res:// of `resources` (resource_system::subscribe_changes) until the database goes.
		void attach(res::resource_system& resources);
		// Applies one batch of changes below res:// (main thread, as resource_system delivers them): every file or
		// .resource named is looked up again in the layers. A file that went away and one that came carrying the same guid
		// in their .resource are a move, whatever the watcher called them; a folder that came or went walks the tree again.
		asset_batch apply_changes(std::span<const res::file_change> changes);
		// Called after each batch with what it did (S57-7 puts the resource_system's own handling after it).
		void set_on_batch(std::function<void(const asset_batch&)> on_batch) { m_on_batch = std::move(on_batch); }

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
		void load_cache();
		// The record `relative` (res:// path text without the protocol) has now; none when no layer has the file.
		std::optional<asset_record> look_up(const std::string& relative, const asset_record* before);
		void reindex();
		void save_when_idle();
		std::filesystem::path cache_file() const;
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
		asset_cache_location m_cache;
		// Shared with a background write, so the lock is held until that write is done.
		std::shared_ptr<platform::file_lock> m_lock;
		// Records read from the cache: what the first walk starts from; not published.
		std::map<std::string, asset_record> m_cached;
		std::uint64_t m_save_job = 0;
		bool m_save_again = false;
		// Shared with a background write: one write at a time, and an older snapshot never replaces a newer one.
		std::shared_ptr<asset_cache_writer> m_writer;
		std::uint64_t m_snapshots = 0;
		bool m_loaded_from_cache = false;
		bool m_dirty = false;
		std::size_t m_files_read = 0;
		res::resource_system* m_resources = nullptr;
		std::function<void(const asset_batch&)> m_on_batch;
		connection m_job_finished;
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}
