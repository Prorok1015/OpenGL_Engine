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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
	// What one batch of changes lists of the layers' folders (asset_database.cpp).
	class asset_layer_lookup;

	// Names that differ only in letter case are one name (Windows).
#if defined(_WIN32)
	inline constexpr bool platform_case_insensitive = true;
#else
	inline constexpr bool platform_case_insensitive = false;
#endif

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
		// The layer whose .resource the record read (any layer may hold it); meaningful only with has_meta.
		std::size_t meta_layer = 0;
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
		// Index into the layers (lowest first) of the layer that holds the .resource.
		std::size_t layer = 0;
		// The content hash of the file the .resource belonged to, when the database knew it; empty otherwise.
		std::string last_hash;

		bool operator==(const asset_orphan&) const = default;
	};

	// What a fix of the asset database (or its undo) came to: `error` says why it did not happen.
	struct asset_fix_result
	{
		bool ok = false;
		std::string error;
		// A failed undo stays in the history: reading, renaming or writing failed, not a file changed since.
		bool keep = false;
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
		// The string "__guid" of a desc's root, as written (not checked).
		std::optional<std::string> root_guid;
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

	// The kind of asset of a path by its suffix (lowercase compare).
	std::string asset_type_of(std::string_view path);

	// Walks `layers` (lowest first, as resource_system::get_resource_layers gives them) merged as the asset browser merges
	// them (an upper layer shadows a name below, case folded when `case_insensitive`), and reads every file's .resource. In
	// every layer, an entry that redirects (a link, a junction) is neither listed nor entered, and hidden folders are
	// skipped. `stopped` (may be empty) ends the walk early.
	// `known` (may be null): records by path text, from the cache or the last walk; a file whose .resource has the size
	// and time recorded there keeps the record's ID and flags without the .resource being read again.
	asset_scan scan_asset_layers(const std::vector<std::filesystem::path>& layers, const std::function<bool()>& stopped = {},
		const std::map<std::string, asset_record>* known = nullptr, bool case_insensitive = platform_case_insensitive);

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
		// Batches that arrive during a walk wait for it, up to this many changes; past it one more walk follows instead.
		static constexpr std::size_t max_deferred_changes = 4096;
		// A larger cache is not read (the walk then reads every file).
		static constexpr std::uint64_t max_cache_bytes = 16u * 1024u * 1024u;
		// A cache file stamp within this of the time it was checked is not trusted (FAT and SMB keep 2 s).
		static constexpr std::chrono::seconds stamp_granularity{ 2 };
		// A git index or HEAD written this recently means a git operation may still be writing files (#765).
		static constexpr std::chrono::seconds git_quiet_time{ 5 };
		// A file from outside written this recently may still be written, copied or moved: it waits for its ID.
		static constexpr std::chrono::seconds new_file_quiet_time{ 5 };
		// A file without ID that went away this recently and one of its size and time that comes are a move.
		static constexpr std::chrono::seconds move_window{ 60 };
		// A file waiting for its hash this long (it cannot be read) stops waiting for an ID.
		static constexpr std::chrono::seconds pending_timeout{ 60 };
		// poll() looks at the files waiting for an ID at most this often.
		static constexpr std::chrono::seconds poll_interval{ 1 };

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
		// Names merged with their letter case folded (default: the platform's). Set before the first rebuild(); for tests
		// of the Windows rules on any system.
		void set_case_insensitive(bool value) { m_case_insensitive = value; }
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
		// .resource named is looked up again in the layers, as the walk would find it. A file that went away and one that
		// came carrying the same guid in their .resource are a move, whatever the watcher called them; a folder that came
		// or went walks the tree again (once more after a running walk). Changes below hidden folders are ignored. While a
		// walk runs (or before the first one), the batch waits and is applied after the walk's result: the return is
		// then empty and on_batch is called when it is applied.
		asset_batch apply_changes(std::span<const res::file_change> changes);
		// Called after each batch with what it did, and after each walk but the first with what it changed (S57-7 puts
		// the resource_system's own handling after it).
		void set_on_batch(std::function<void(const asset_batch&)> on_batch) { m_on_batch = std::move(on_batch); }

		// New files get an ID (#765, EPIC-57 section 6.1): a file of the top layer without a .resource in any layer that
		// a batch after the first walk reports as created, or a later walk adds, gets <file>.resource with a new ID (a
		// desc: the valid "__guid" of its root, read by the hashing pass, unless another file has that ID). Not for OS
		// litter or temporaries, nor for what matches a file without ID that went away (a move). The editor's own write
		// (file_change::self) gets it in that batch (a desc once hashed); a file from outside waits for its hash and to
		// be new_file_quiet_time old, and gets none when an orphan had that hash (a move: "Link" in Problems), nor while
		// a git operation runs. Files the first walk finds are left as they are (migration).
		// `busy` (may be empty): the editor's own git operation is running.
		void set_vcs_busy(std::function<bool()> busy) { m_vcs_busy = std::move(busy); }
		// The repository's git folder (index, HEAD), asked at every check (may be empty: no git folder is looked at).
		void set_git_dir_provider(std::function<std::optional<std::filesystem::path>()> provider) { m_git_dir = std::move(provider); }
		// Looks at the files waiting for an ID again (a git operation may have ended); at most every poll_interval
		// unless `force`. Cheap when none waits. Main thread, once per frame.
		void poll(bool force = false);
		// Files from outside waiting for an ID. For tests.
		std::size_t pending_ids() const { return m_pending_ids.size(); }

		const asset_record* find(const res::tag& path) const;
		std::vector<const asset_record*> find(const res::asset_id& id) const;
		const std::map<std::string, asset_record>& records() const { return m_records; }
		// IDs that more than one file carries.
		std::vector<res::asset_id> duplicates() const;
		const std::vector<asset_orphan>& orphans() const { return m_orphans; }
		// Grows whenever anything the database holds changed.
		std::uint64_t revision() const { return m_revision; }
		const std::vector<std::filesystem::path>& layers() const { return m_layers; }
		// The project's own layer (the highest): the only one the fixes below write to.
		std::size_t top_layer() const { return m_layers.empty() ? 0 : m_layers.size() - 1; }

		// Fixes for the person (Problems, S57-4), never for an agent. Each one is applied to the database at once (as a
		// batch of changes, so set_on_batch hears of it) and can be undone through the short history below.
		// Writes <file>.resource with a new ID for a file of the top layer that has no .resource in any layer; never
		// replaces one.
		asset_fix_result create_meta(const res::tag& file);
		// Renames the orphan .resource `meta` to <file>.resource, both in the top layer (an orphan of a lower layer is
		// refused); never replaces one.
		asset_fix_result link_orphan(const res::tag& meta, const res::tag& file);
		// Gives the .resource of `file` (in the top layer) a new ID, keeping everything else it holds.
		asset_fix_result new_id_for_copy(const res::tag& file);

		static constexpr std::size_t max_history = 32;
		// The label of the fix undo_last() would take back; none when there is nothing to undo.
		std::optional<std::string> next_undo_label() const;
		// Takes back the last fix and forgets it. Refused (and forgotten) when its file changed since the fix.
		asset_fix_result undo_last();

	private:
		struct history_entry
		{
			std::string label;
			std::function<asset_fix_result()> undo;
		};

		void push_history(std::string label, std::function<asset_fix_result()> undo);
		// Looks up the files (res:// path text without the protocol; a .resource stands for its file) again, their
		// .resource read again whatever its stamp says: at once, or, while a walk runs, as a batch deferred until it ends
		// (the fix is on the disk at once and shows then).
		void refresh(const std::vector<std::string>& relatives);
		// An orphan of `meta_path` made without a known hash takes this one (an undone link, possibly deferred).
		std::string take_orphan_hash(const res::tag& meta_path);
		void apply_scan(asset_scan scan);
		enum class walk_end { applied, failed, cancelled };
		// After a walk ended: the batches that waited, then the walk asked for meanwhile; a cancelled walk is started
		// again once.
		void after_walk(walk_end end);
		// Pairs `gone` and `came` (keys of m_records) by guid into `batch` (moved, added, removed).
		void pair_moves(std::vector<asset_record> gone, const std::vector<std::string>& came, asset_batch& batch);
		void hash_next();
		// m_by_id and, case insensitive, m_by_folded.
		void index_id(const asset_record& record);
		void unindex_id(const asset_record& record);
		void on_job_finished(std::uint64_t id, job_status status);
		void load_cache();
		// The record the walk would make of `relative` (res:// path text without the protocol, any letter case when case
		// insensitive) now; none when it would make none. `before_of` gives what is known of a path text (may be null).
		std::optional<asset_record> look_up(const std::string& relative, asset_layer_lookup& lookup,
			const std::function<const asset_record*(const std::string&)>& before_of);
		void save_when_idle();
		// Writes the cache in the background when something changed (a write already running: once more after it).
		void start_save();
		std::filesystem::path cache_file() const;
		// The keys of the hashing job that ended without an answer for them go back into the queue.
		void requeue_in_flight();
		// Writes the .resource of the record `key` with a new ID when it still has none in any layer (not a fix: no
		// history).
		void give_id(const std::string& key);
		// Gives an ID to each file waiting for one that is ready; drops those that went or got a .resource.
		void give_pending_ids();
		// A git operation may be writing files: the editor's own, or one seen in the repository's git folder.
		bool git_busy();
		// The record `key` came after the first walk: it waits for an ID when it is a new file of the top layer.
		void queue_new_file(const std::string& key, bool own);
		// Records without ID that went away: what comes with their size and time is a move.
		void remember_gone(const std::vector<asset_record>& gone);
		bool matches_gone(const asset_record& record);

		job_service& m_jobs;
		std::vector<std::filesystem::path> m_layers;
		std::map<std::string, asset_record> m_records; // by path text
		std::multimap<res::asset_id, std::string> m_by_id;
		// Path text by its merge key, case insensitive only: finds a record under another letter case.
		std::unordered_multimap<std::string, std::string> m_by_folded;
		std::vector<asset_orphan> m_orphans;
		std::vector<std::string> m_unhashed;
		std::vector<std::string> m_in_flight;
		std::uint64_t m_hash_job = 0;
		std::uint64_t m_scan_job = 0;
		bool m_scanning = false;
		// A walk's result was applied: m_records is what the layers hold, not m_cached.
		bool m_walked_once = false;
		// A folder changed during a walk: one more walk follows it.
		bool m_walk_again = false;
		// The last walk was cancelled and started again; cleared by an applied walk.
		bool m_walk_retried = false;
		// File changes that came during a walk (or before the first), applied after it.
		std::vector<res::file_change> m_deferred;
		bool m_case_insensitive = platform_case_insensitive;
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
		std::vector<history_entry> m_history;
		// Records (by path text) whose .resource the next look-up reads whatever its stamp: a fix rewrote it, possibly
		// within one tick of a coarse clock.
		std::set<std::string> m_force_meta;
		// By merge key of the .resource path text: last_hash for the orphan an undone link brings back (take_orphan_hash).
		std::map<std::string, std::string> m_orphan_hashes;
		struct pending_id
		{
			std::chrono::steady_clock::time_point since;
			// Written by the editor: no wait for the quiet time, an orphan or git.
			bool own = false;
		};
		// Records (by path text) waiting for an ID: for their hash, quiet time or the end of a git operation.
		std::map<std::string, pending_id> m_pending_ids;
		// A waiting desc's root "__guid" from the hashing pass (none: it has no string one).
		std::map<std::string, std::optional<std::string>> m_root_guids;
		struct gone_file
		{
			std::int64_t size = 0;
			std::int64_t mtime = 0;
			std::string content_hash;
			std::chrono::steady_clock::time_point at;
		};
		std::vector<gone_file> m_gone_without_id;
		std::function<bool()> m_vcs_busy;
		std::function<std::optional<std::filesystem::path>()> m_git_dir;
		// An ID is being written: the batch its refresh applies gives none itself.
		bool m_giving_ids = false;
		std::chrono::steady_clock::time_point m_last_poll{};
		connection m_job_finished;
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}
