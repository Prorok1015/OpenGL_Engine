#include "edt_asset_database.h"
#include "edt_asset_layers.h"
#include <snk/editor/edt_job_service.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/project/prj_json_text.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_system.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <snk/platform/plt_reparse_tag.h>
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr std::uint64_t k_meta_max_bytes = 64u * 1024u;

	std::uint64_t fnv1a_64_update(std::uint64_t hash, std::span<const std::byte> bytes)
	{
		for (const std::byte b : bytes) {
			hash ^= static_cast<std::uint64_t>(b);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// A .resource sidecar by the one rule of the editor (any letter case), with a name before the suffix.
	bool is_sidecar(std::string_view relative)
	{
		const std::string_view name = relative.substr(relative.find_last_of('/') + 1);
		return name.size() > res::asset_meta_suffix.size() && res::is_asset_meta_name(name);
	}

	// The path of the file a sidecar belongs to.
	std::string_view sidecar_owner(std::string_view relative)
	{
		return relative.substr(0, relative.size() - res::asset_meta_suffix.size());
	}

	struct file_stat
	{
		bool ok = false;
		std::int64_t size = 0;
		std::int64_t mtime = 0;

		bool operator==(const file_stat&) const = default;
	};

	std::int64_t now_ticks()
	{
		return static_cast<std::int64_t>(fs::file_time_type::clock::now().time_since_epoch().count());
	}

	// A regular file that is not a link.
	file_stat stat_of(const fs::path& path)
	{
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(path, ec);
		if (ec || !fs::is_regular_file(status)) {
			return {};
		}
		file_stat out;
		out.size = static_cast<std::int64_t>(fs::file_size(path, ec));
		if (ec) {
			return {};
		}
		const auto time = fs::last_write_time(path, ec);
		if (ec) {
			return {};
		}
		out.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
		out.ok = true;
		return out;
	}

	// An entry the walk neither lists nor enters: a link, or on Windows a junction or any reparse point other than one a
	// file system filter keeps in place (a OneDrive placeholder is the file itself).
	bool redirects(const fs::directory_entry& entry)
	{
		std::error_code ec;
		if (entry.is_symlink(ec) || ec) {
			return true;
		}
#if defined(_WIN32)
		WIN32_FIND_DATAW data{};
		// The \\?\ prefix lifts MAX_PATH for an absolute path, and turns off the normalization that would make a '/' a
		// separator: the path goes with '\' only.
		std::wstring query = fs::path(entry.path()).make_preferred().native();
		if (entry.path().is_absolute() && !query.starts_with(LR"(\\?\)")) {
			query.insert(0, LR"(\\?\)");
		}
		const HANDLE found = ::FindFirstFileW(query.c_str(), &data);
		if (found == INVALID_HANDLE_VALUE) {
			egLOG_WARN("assets", "{}: cannot be inspected ({}), left out of the asset database", fs_paths::utf8_of(entry.path()),
				std::system_category().message(static_cast<int>(::GetLastError())));
			return true;
		}
		::FindClose(found);
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
			return false;
		}
		return platform::beneath::reparse_tag_redirects(data.dwReserved0) || !platform::beneath::reparse_tag_kept_in_place(data.dwReserved0);
#else
		return false;
#endif
	}

	struct merged_file
	{
		fs::path physical;
		fs::path relative;
		std::size_t layer = 0;
		std::vector<std::size_t> shadowed;
	};

	struct walk_state
	{
		const std::vector<fs::path>& layers;
		const std::function<bool()>& stopped;
		std::vector<merged_file>& out;
		bool case_insensitive = false;
		bool depth_warned = false;
	};

	// One folder of the merged tree. `in_layers`: the layers (ascending) where `relative_dir` is a real folder.
	void walk(walk_state& state, const fs::path& relative_dir, const std::vector<std::size_t>& in_layers, int depth)
	{
		if (depth >= edt::asset_database::max_depth) {
			if (!state.depth_warned) {
				egLOG_WARN("assets", "res://{}: deeper than {} folders, not walked further", fs_paths::generic_utf8_of(relative_dir),
					edt::asset_database::max_depth);
				state.depth_warned = true;
			}
			return;
		}
		struct slot
		{
			fs::path relative;
			fs::path physical;
			bool is_dir = false;
			std::size_t layer = 0;
			std::vector<std::size_t> dir_layers;
			std::vector<std::size_t> shadowed;
		};
		std::map<std::string, slot> merged;
		for (const std::size_t layer : in_layers) {
			std::error_code ec;
			fs::directory_iterator it(state.layers[layer] / relative_dir, fs::directory_options::skip_permission_denied, ec);
			for (; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
				if (state.stopped && state.stopped()) {
					return;
				}
				const fs::directory_entry& entry = *it;
				if (redirects(entry)) {
					continue;
				}
				std::error_code kind_ec;
				const bool is_dir = entry.is_directory(kind_ec);
				if (kind_ec) {
					continue;
				}
				const std::string name = fs_paths::utf8_of(entry.path().filename());
				if (is_dir && name.starts_with('.')) {
					continue;
				}
				slot& found = merged[edt::layer_merge_key(name, state.case_insensitive)];
				const bool existed = !found.relative.empty();
				if (existed) {
					found.shadowed.push_back(found.layer);
				}
				if (is_dir && existed && found.is_dir) {
					found.dir_layers.push_back(layer);
				} else if (is_dir) {
					found.dir_layers = { layer };
				} else {
					found.dir_layers.clear();
				}
				found.relative = relative_dir / entry.path().filename();
				found.physical = entry.path();
				found.is_dir = is_dir;
				found.layer = layer;
			}
		}
		for (auto& [key, entry] : merged) {
			if (state.stopped && state.stopped()) {
				return;
			}
			if (entry.is_dir) {
				walk(state, entry.relative, entry.dir_layers, depth + 1);
			} else {
				state.out.push_back(merged_file{ std::move(entry.physical), std::move(entry.relative), entry.layer, std::move(entry.shadowed) });
			}
		}
	}

	struct meta_read
	{
		bool present = false;
		std::optional<res::asset_meta> meta;
		std::string error;
	};

	meta_read read_meta(const fs::path& layer_root, const fs::path& relative)
	{
		meta_read out;
		platform::beneath::read_request request;
		request.root = layer_root;
		request.folder = relative.parent_path();
		request.leaf = relative.filename();
		request.max_bytes = k_meta_max_bytes;
		std::vector<std::byte> bytes;
		const platform::beneath::read_result result = platform::beneath::read_beneath(request, bytes);
		if (result.status == platform::beneath::read_status::missing) {
			return out;
		}
		out.present = true;
		if (result.status != platform::beneath::read_status::ok) {
			out.error = result.error.empty() ? std::string{ "cannot be read" } : result.error;
			return out;
		}
		const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		res::asset_meta_result read = res::read_asset_meta(text);
		if (!read) {
			out.error = read.error.field_path.empty() ? read.error.message : read.error.field_path + ": " + read.error.message;
			return out;
		}
		out.meta = std::move(*read.meta);
		return out;
	}
}

std::string edt::asset_type_of(std::string_view path)
{
	std::string lower{ path };
	std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const auto has = [&lower](std::string_view suffix) { return lower.ends_with(suffix); };
	if (has(".desc")) {
		return "desc";
	}
	for (const std::string_view suffix : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".ktx", ".ktx2", ".dds" }) {
		if (has(suffix)) {
			return "texture";
		}
	}
	for (const std::string_view suffix : { ".glb", ".gltf", ".obj", ".fbx" }) {
		if (has(suffix)) {
			return "model";
		}
	}
	for (const std::string_view suffix : { ".glsl", ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese", ".shader" }) {
		if (has(suffix)) {
			return "shader";
		}
	}
	return "file";
}

edt::asset_scan edt::scan_asset_layers(const std::vector<fs::path>& layers, const std::function<bool()>& stopped,
	const std::map<std::string, asset_record>* known, bool case_insensitive)
{
	std::vector<merged_file> files;
	std::vector<std::size_t> all_layers(layers.size());
	for (std::size_t i = 0; i < layers.size(); ++i) {
		all_layers[i] = i;
	}
	walk_state state{ layers, stopped, files, case_insensitive, false };
	walk(state, fs::path{}, all_layers, 0);

	// Keyed by the file they belong to, as the merge keys names: "a.png.RESOURCE" belongs to "a.png" everywhere, and on
	// Windows "A.png.resource" to "a.png" too.
	std::unordered_map<std::string, const merged_file*> metas;
	for (const merged_file& file : files) {
		const std::string relative = fs_paths::generic_utf8_of(file.relative);
		if (is_sidecar(relative)) {
			metas.emplace(edt::layer_merge_key(std::string{ sidecar_owner(relative) }, case_insensitive), &file);
		}
	}

	asset_scan scan;
	scan.records.reserve(files.size() - metas.size());
	std::unordered_set<std::string> paired;
	for (const merged_file& file : files) {
		if (stopped && stopped()) {
			return scan;
		}
		const std::string relative = fs_paths::generic_utf8_of(file.relative);
		if (is_sidecar(relative)) {
			continue;
		}
		const std::int64_t checked_at = now_ticks();
		const file_stat stat = stat_of(file.physical);
		if (!stat.ok) {
			continue;
		}
		asset_record record;
		record.content_checked_at = checked_at;
		record.meta_checked_at = checked_at;
		record.path = res::tag::make(relative);
		record.layer = file.layer;
		record.shadowed_layers = file.shadowed;
		record.size = stat.size;
		record.mtime = stat.mtime;
		record.type = asset_type_of(relative);
		const std::string meta_key = edt::layer_merge_key(relative, case_insensitive);
		if (const auto meta = metas.find(meta_key); meta != metas.end()) {
			paired.insert(meta_key);
			const file_stat meta_stat = stat_of(meta->second->physical);
			record.meta_size = meta_stat.size;
			record.meta_mtime = meta_stat.mtime;
			const asset_record* before = nullptr;
			if (known) {
				if (const auto found = known->find(record.path.string()); found != known->end()) {
					before = &found->second;
				}
			}
			if (before && before->has_meta && before->meta_size == meta_stat.size && before->meta_mtime == meta_stat.mtime) {
				record.has_meta = true;
				record.id = before->id;
				record.flags = before->flags;
				record.meta_error = before->meta_error;
				record.meta_checked_at = std::min(record.meta_checked_at, before->meta_checked_at);
				scan.records.push_back(std::move(record));
				continue;
			}
			++scan.meta_reads;
			const meta_read read = read_meta(layers[meta->second->layer], meta->second->relative);
			record.has_meta = read.present;
			if (read.meta) {
				record.id = read.meta->guid;
				record.flags = read.meta->flags;
			} else {
				record.meta_error = read.error;
			}
		}
		scan.records.push_back(std::move(record));
	}

	for (const auto& [key, file] : metas) {
		if (paired.contains(key)) {
			continue;
		}
		asset_orphan orphan;
		orphan.meta_path = res::tag::make(fs_paths::generic_utf8_of(file->relative));
		++scan.meta_reads;
		const meta_read read = read_meta(layers[file->layer], file->relative);
		if (read.meta) {
			orphan.id = read.meta->guid;
		}
		scan.orphans.push_back(std::move(orphan));
	}
	std::ranges::sort(scan.orphans, {}, [](const asset_orphan& orphan) { return orphan.meta_path.string(); });
	return scan;
}

edt::asset_content edt::read_asset_content(const fs::path& layer_root, const fs::path& relative, bool is_desc,
	std::uint64_t max_desc_bytes)
{
	asset_content out;
	const file_stat before = stat_of(layer_root / relative);
	platform::beneath::read_request request;
	request.root = layer_root;
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = (std::numeric_limits<std::uint64_t>::max)();

	std::uint64_t hash = 14695981039346656037ull;
	std::string text;
	bool keep_text = is_desc;
	const platform::beneath::read_result result = platform::beneath::read_beneath_stream(request,
		[&](std::span<const std::byte> chunk) {
			hash = fnv1a_64_update(hash, chunk);
			if (keep_text) {
				if (text.size() + chunk.size() > max_desc_bytes) {
					keep_text = false;
					text.clear();
				} else {
					text.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
				}
			}
			return true;
		});
	if (result.status != platform::beneath::read_status::ok) {
		out.error = result.error.empty() ? std::string{ "cannot be read" } : result.error;
		return out;
	}
	const file_stat after = stat_of(layer_root / relative);
	out.ok = true;
	out.size = after.size;
	out.mtime = after.mtime;
	out.unchanged = before.ok && before == after;
	out.content_hash = std::format("fnv1a64:{:016x}", hash);
	if (keep_text) {
		boost::system::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 64;
		const boost::json::value document = boost::json::parse(text, ec, {}, options);
		if (!ec && document.is_object()) {
			if (const boost::json::value* type = document.as_object().if_contains("__type"); type && type->is_string()) {
				out.desc_type = std::string{ type->get_string() };
			}
		}
	}
	return out;
}

struct edt::asset_cache_writer
{
	// Writes snapshot number `snapshot` unless a newer one was written already; the error, nullopt on success (a skip
	// is a success).
	std::optional<std::string> write(std::uint64_t snapshot, const std::function<std::optional<std::string>()>& write_file)
	{
		const std::scoped_lock hold(mutex);
		if (snapshot <= written) {
			return std::nullopt;
		}
		std::optional<std::string> error = write_file();
		if (!error) {
			written = snapshot;
		}
		return error;
	}

	std::mutex mutex;
	std::uint64_t written = 0;
};

bool edt::asset_record::same_as(const asset_record& other) const
{
	return path == other.path && layer == other.layer && shadowed_layers == other.shadowed_layers && id == other.id
		&& size == other.size && mtime == other.mtime && has_meta == other.has_meta && meta_size == other.meta_size
		&& meta_mtime == other.meta_mtime && meta_error == other.meta_error && flags == other.flags && type == other.type
		&& content_hash == other.content_hash && desc_type == other.desc_type;
}

edt::asset_database::asset_database(job_service& jobs, editor_events* events, std::vector<fs::path> layers, asset_cache_location cache)
	: m_jobs(jobs)
	, m_cache(std::move(cache))
	, m_writer(std::make_shared<asset_cache_writer>())
{
	m_layers.reserve(layers.size());
	for (fs::path& layer : layers) {
		std::error_code ec;
		fs::path canonical = fs::weakly_canonical(layer, ec);
		// '\' only on Windows: the walk asks FindFirstFileW with the \\?\ prefix, which takes no '/'.
		m_layers.push_back((ec ? std::move(layer) : std::move(canonical)).make_preferred());
	}
	if (!m_cache.root.empty()) {
		std::string error;
		if (std::optional<platform::file_lock> lock = platform::file_lock::try_acquire(m_cache.root, m_cache.relative / ".lock", &error)) {
			m_lock = std::make_shared<platform::file_lock>(std::move(*lock));
		} else if (error.empty()) {
			egLOG_INFO("assets", "{}: the asset database cache is held by another editor; this one keeps its database in memory",
				fs_paths::utf8_of(cache_file()));
		} else {
			egLOG_WARN("assets", "the asset database cache cannot be locked ({}); the database is kept in memory", error);
		}
		load_cache();
	}
	if (events) {
		m_job_finished = events->job_finished.subscribe(
			[this](std::uint64_t id, job_status status, const std::string&, const std::optional<boost::json::value>&) {
				on_job_finished(id, status);
			});
	}
}

edt::asset_database::~asset_database()
{
	if (m_resources) {
		m_resources->unsubscribe_changes(this);
	}
	try {
		// A background write may be cancelled with the database or still running; save() waits for a running one and
		// writes what is newest.
		if (m_save_job != 0) {
			m_dirty = true;
		}
		save();
	} catch (const std::exception& error) {
		egLOG_WARN("assets", "the asset database cache was not written: {}", error.what());
	}
}

void edt::asset_database::rebuild()
{
	// Cleared first: the cancelled walk's end is not taken for this one's.
	if (const std::uint64_t running = std::exchange(m_scan_job, 0); m_scanning && running != 0) {
		m_jobs.cancel(running);
	}
	// This walk sees every folder as it is now.
	m_walk_again = false;
	// A hashing job someone cancelled without the database seeing it (no events): its keys go back into the queue.
	if (m_hash_job != 0 && !m_jobs.is_running(m_hash_job)) {
		requeue_in_flight();
	}
	const std::uint64_t number = ++m_scan_number;
	m_scanning = true;
	m_scan_job = m_jobs.start(ds::lane::io, ds::task_desc{ "Scan assets", "assets", true }, m_lifetime,
		[layers = m_layers, known = m_walked_once ? m_records : m_cached, case_insensitive = m_case_insensitive](ds::task_context& ctx) {
			return scan_asset_layers(layers, [&ctx] { return ctx.stop.stop_requested(); }, &known, case_insensitive);
		},
		[this, number](ds::task_result<asset_scan>& result) {
			if (number != m_scan_number) {
				return;
			}
			m_scanning = false;
			m_scan_job = 0;
			if (!result.ok()) {
				egLOG_WARN("assets", "the walk of the asset tree failed: {}", result.error);
				after_walk(false);
				return;
			}
			apply_scan(std::move(*result.value));
			after_walk(true);
		});
}

void edt::asset_database::after_walk(bool applied)
{
	// The walk listed a folder before or after these changes: applied again, they hold whatever it saw. Before the
	// first walk was applied they wait on (apply_changes defers them again).
	if (!m_deferred.empty()) {
		const std::vector<res::file_change> pending = std::exchange(m_deferred, {});
		apply_changes(pending);
	}
	// A walk that failed or was cancelled is not restarted against it: the next rebuild covers the folders.
	if (std::exchange(m_walk_again, false) && applied) {
		rebuild();
	}
}

void edt::asset_database::on_job_finished(std::uint64_t id, job_status status)
{
	if (status != job_status::cancelled) {
		return;
	}
	if (id != 0 && id == m_scan_job) {
		m_scanning = false;
		m_scan_job = 0;
		after_walk(false);
	}
	if (id != 0 && id == m_save_job) {
		m_save_job = 0;
		m_dirty = true;
	}
	if (id != 0 && id == m_hash_job) {
		// Stopped from outside (job.cancel): the files wait for the next rebuild, not for a restart against the cancel.
		requeue_in_flight();
	}
}

void edt::asset_database::requeue_in_flight()
{
	for (std::string& key : m_in_flight) {
		if (const auto found = m_records.find(key); found != m_records.end() && found->second.content_hash.empty()) {
			m_unhashed.push_back(std::move(key));
		}
	}
	m_in_flight.clear();
	m_hash_job = 0;
}

void edt::asset_database::apply_scan(asset_scan scan)
{
	// The first walk starts from the cache; later ones from what the last one found.
	const bool first_walk = !m_walked_once;
	m_walked_once = true;
	std::map<std::string, asset_record> previous = first_walk ? std::move(m_cached) : std::move(m_records);
	m_cached.clear();
	m_records.clear();
	m_by_id.clear();
	m_unhashed.clear();
	for (asset_record& record : scan.records) {
		std::string key = record.path.string();
		// A file the walk found as it was keeps what the background pass learned of it.
		if (const auto known = previous.find(key);
			known != previous.end() && !known->second.content_hash.empty() && known->second.size == record.size
			&& known->second.mtime == record.mtime) {
			record.content_hash = known->second.content_hash;
			record.desc_type = known->second.desc_type;
			// The hash is as old as that record's check, so a stamp it could not trust stays untrusted. Within a session a
			// record with the same stamp is carried over without the racy check; a restart catches it through this min.
			record.content_checked_at = std::min(record.content_checked_at, known->second.content_checked_at);
		}
		const bool in_flight = std::ranges::find(m_in_flight, key) != m_in_flight.end();
		if (record.content_hash.empty() && !in_flight) {
			m_unhashed.push_back(key);
		}
		index_id(record);
		m_records.emplace(std::move(key), std::move(record));
	}
	const bool same_records = std::ranges::equal(previous, m_records,
		[](const auto& a, const auto& b) { return a.first == b.first && a.second.same_as(b.second); });
	const bool same_orphans = scan.orphans == m_orphans;
	m_orphans = std::move(scan.orphans);
	m_files_read += scan.meta_reads;
	if (!same_records) {
		m_dirty = true;
	}
	if (!same_records || !same_orphans || first_walk) {
		++m_revision;
	}
	hash_next();
	save_when_idle();
}

void edt::asset_database::index_id(const asset_record& record)
{
	if (record.id) {
		m_by_id.emplace(*record.id, record.path.string());
	}
}

void edt::asset_database::unindex_id(const asset_record& record)
{
	if (!record.id) {
		return;
	}
	const std::string key = record.path.string();
	const auto [first, last] = m_by_id.equal_range(*record.id);
	for (auto it = first; it != last; ++it) {
		if (it->second == key) {
			m_by_id.erase(it);
			return;
		}
	}
}

void edt::asset_database::hash_next()
{
	if (m_hash_job != 0 || m_unhashed.empty()) {
		return;
	}
	struct item
	{
		std::string key;
		fs::path root;
		fs::path relative;
		bool is_desc = false;
		// The record's stamp when it was queued.
		std::int64_t size = 0;
		std::int64_t mtime = 0;
	};
	std::vector<item> batch;
	while (!m_unhashed.empty() && batch.size() < hash_batch) {
		std::string key = std::move(m_unhashed.back());
		m_unhashed.pop_back();
		const auto found = m_records.find(key);
		if (found == m_records.end() || !found->second.content_hash.empty()) {
			continue;
		}
		const asset_record& record = found->second;
		std::string_view relative = record.path.view();
		relative.remove_prefix(relative.find("://") + 3);
		m_in_flight.push_back(key);
		batch.push_back(item{ std::move(key), m_layers[record.layer], fs_paths::path_from_utf8(relative), record.type == "desc", record.size,
			record.mtime });
	}
	if (batch.empty()) {
		return;
	}
	struct hashed
	{
		std::string key;
		std::int64_t size = 0;
		std::int64_t mtime = 0;
		asset_content content;
	};
	using results = std::vector<hashed>;
	m_hash_job = m_jobs.start(ds::lane::io, ds::task_desc{ "Hash assets", "assets", true }, m_lifetime,
		[batch = std::move(batch)](ds::task_context& ctx) {
			results out;
			out.reserve(batch.size());
			for (const item& entry : batch) {
				if (ctx.stop.stop_requested()) {
					break;
				}
				out.push_back(hashed{ entry.key, entry.size, entry.mtime, read_asset_content(entry.root, entry.relative, entry.is_desc, max_desc_bytes) });
			}
			return out;
		},
		[this](ds::task_result<results>& result) {
			if (result.ok()) {
				bool changed = false;
				const auto queue_again = [this](const std::string& key) {
					if (std::ranges::find(m_unhashed, key) == m_unhashed.end()) {
						m_unhashed.push_back(key);
					}
				};
				for (auto& [key, queued_size, queued_mtime, content] : *result.value) {
					std::erase(m_in_flight, key);
					++m_files_read;
					const auto found = m_records.find(key);
					if (found == m_records.end()) {
						continue;
					}
					// A batch or a walk gave the record a new stamp while it was read (it skipped the key in flight).
					if (found->second.size != queued_size || found->second.mtime != queued_mtime) {
						if (found->second.content_hash.empty()) {
							queue_again(key);
						}
						continue;
					}
					if (!content.ok) {
						egLOG_WARN("assets", "{}: {}", key, content.error);
						continue;
					}
					// Changed on the disk since the record: the next walk or batch of changes updates it and queues it again.
					if (found->second.size != content.size || found->second.mtime != content.mtime) {
						continue;
					}
					// Written while it was read, now as the record says: hashed again with the next batch.
					if (!content.unchanged) {
						queue_again(key);
						continue;
					}
					found->second.content_hash = std::move(content.content_hash);
					found->second.desc_type = std::move(content.desc_type);
					changed = true;
				}
				if (changed) {
					m_dirty = true;
					++m_revision;
				}
			} else {
				// Not retried at once (it would fail again in a loop): the files wait for the next rebuild.
				egLOG_WARN("assets", "hashing assets failed: {}", result.error);
				requeue_in_flight();
				return;
			}
			// What the job did not get to (stopped half way) is hashed with the next batch.
			requeue_in_flight();
			hash_next();
			save_when_idle();
		});
}

std::size_t edt::asset_database::pending_hashes() const
{
	return m_unhashed.size() + m_in_flight.size();
}

const edt::asset_record* edt::asset_database::find(const res::tag& path) const
{
	const auto found = m_records.find(path.string());
	return found == m_records.end() ? nullptr : &found->second;
}

std::vector<const edt::asset_record*> edt::asset_database::find(const res::asset_id& id) const
{
	std::vector<const asset_record*> out;
	const auto [first, last] = m_by_id.equal_range(id);
	for (auto it = first; it != last; ++it) {
		if (const asset_record* record = find(res::tag{ it->second })) {
			out.push_back(record);
		}
	}
	return out;
}

std::vector<res::asset_id> edt::asset_database::duplicates() const
{
	std::vector<res::asset_id> out;
	for (auto it = m_by_id.begin(); it != m_by_id.end();) {
		const auto next = m_by_id.upper_bound(it->first);
		if (std::distance(it, next) > 1) {
			out.push_back(it->first);
		}
		it = next;
	}
	return out;
}

void edt::asset_database::save_when_idle()
{
	if (!m_lock || !m_dirty || m_scanning || m_hash_job != 0 || !m_unhashed.empty()) {
		return;
	}
	if (m_save_job != 0) {
		m_save_again = true;
		return;
	}
	m_dirty = false;
	// Copied here, serialized and written on the io lane: the main thread never waits for the disk.
	m_save_job = m_jobs.start(ds::lane::io, ds::task_desc{ "Save the asset database", "assets", false }, m_lifetime,
		[cache = m_cache, lock = m_lock, writer = m_writer, snapshot = ++m_snapshots, layers = m_layers, records = m_records](ds::task_context&) {
			(void)lock;
			return writer->write(snapshot, [&] { return write_cache(cache, layers, records); });
		},
		[this](ds::task_result<std::optional<std::string>>& result) {
			m_save_job = 0;
			const std::optional<std::string> error = result.ok() ? *result.value : std::optional<std::string>{ result.error };
			if (error) {
				egLOG_WARN("assets", "the asset database cache was not written: {}", *error);
				m_dirty = true;
				return;
			}
			if (std::exchange(m_save_again, false)) {
				save_when_idle();
			}
		});
}

fs::path edt::asset_database::cache_file() const
{
	return m_cache.root / m_cache.relative / "assets.json";
}

namespace
{
	constexpr std::string_view k_cache_type = "asset_db";
	// Raise it with any change of the hash, of how desc_type is found or of this format: an older cache is then
	// dropped, never read with other meanings.
	constexpr std::int64_t k_cache_version = 2;
	constexpr std::string_view k_hash_prefix = "fnv1a64:";

	boost::json::array layer_texts(const std::vector<fs::path>& layers)
	{
		boost::json::array out;
		for (const fs::path& layer : layers) {
			out.emplace_back(fs_paths::generic_utf8_of(layer));
		}
		return out;
	}

	boost::json::value to_json(const edt::asset_record& record)
	{
		boost::json::object out;
		out["path"] = record.path.view();
		out["layer"] = record.layer;
		boost::json::array shadowed;
		for (const std::size_t layer : record.shadowed_layers) {
			shadowed.emplace_back(layer);
		}
		out["shadowed"] = std::move(shadowed);
		if (record.id) {
			out["id"] = res::to_text(*record.id);
		}
		out["size"] = record.size;
		out["mtime"] = record.mtime;
		out["has_meta"] = record.has_meta;
		out["meta_size"] = record.meta_size;
		out["meta_mtime"] = record.meta_mtime;
		if (!record.meta_error.empty()) {
			out["meta_error"] = record.meta_error;
		}
		boost::json::array flags;
		for (const std::string& flag : record.flags) {
			flags.emplace_back(flag);
		}
		out["flags"] = std::move(flags);
		out["type"] = record.type;
		out["hash"] = record.content_hash;
		if (record.desc_type) {
			out["desc_type"] = *record.desc_type;
		}
		out["content_checked_at"] = record.content_checked_at;
		out["meta_checked_at"] = record.meta_checked_at;
		return out;
	}

	// A res:// path as the walk makes them: plain names joined by '/', none empty, "." or "..", no '\' or ':'.
	bool is_walk_path(std::string_view text)
	{
		constexpr std::string_view protocol = "res://";
		if (!text.starts_with(protocol)) {
			return false;
		}
		text.remove_prefix(protocol.size());
		if (text.empty()) {
			return false;
		}
		while (true) {
			const std::size_t slash = text.find('/');
			const std::string_view name = text.substr(0, slash);
			if (name.empty() || name == "." || name == ".." || name.find_first_of("\\:") != std::string_view::npos) {
				return false;
			}
			if (slash == std::string_view::npos) {
				return true;
			}
			text.remove_prefix(slash + 1);
		}
	}

	bool is_hash_text(std::string_view text)
	{
		if (text.empty()) {
			return true;
		}
		if (!text.starts_with(k_hash_prefix) || text.size() != k_hash_prefix.size() + 16) {
			return false;
		}
		return std::ranges::all_of(text.substr(k_hash_prefix.size()), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}

	// A record as save() wrote it, every field checked; none when anything is off (the whole cache is dropped then).
	std::optional<edt::asset_record> record_of(const boost::json::value& value, std::size_t layer_count)
	{
		const boost::json::object* object = value.if_object();
		if (!object) {
			return std::nullopt;
		}
		const auto text = [object](std::string_view key) -> std::optional<std::string> {
			const boost::json::value* field = object->if_contains(key);
			if (!field || !field->is_string()) {
				return std::nullopt;
			}
			return std::string{ field->get_string() };
		};
		const auto integer = [object](std::string_view key) -> std::optional<std::int64_t> {
			const boost::json::value* field = object->if_contains(key);
			if (!field) {
				return std::nullopt;
			}
			if (field->is_int64()) {
				return field->get_int64();
			}
			if (field->is_uint64() && field->get_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
				return static_cast<std::int64_t>(field->get_uint64());
			}
			return std::nullopt;
		};
		const auto path = text("path");
		const auto layer = integer("layer");
		const auto size = integer("size");
		const auto mtime = integer("mtime");
		const auto meta_size = integer("meta_size");
		const auto meta_mtime = integer("meta_mtime");
		const auto content_checked_at = integer("content_checked_at");
		const auto meta_checked_at = integer("meta_checked_at");
		const auto type = text("type");
		const auto hash = text("hash");
		const boost::json::value* has_meta = object->if_contains("has_meta");
		const boost::json::value* shadowed = object->if_contains("shadowed");
		const boost::json::value* flags = object->if_contains("flags");
		if (!path || !is_walk_path(*path) || !layer || *layer < 0 || static_cast<std::size_t>(*layer) >= layer_count || !size || *size < 0
			|| !mtime || !meta_size || !meta_mtime || !content_checked_at || !meta_checked_at || !type || !hash || !is_hash_text(*hash) || !has_meta
			|| !has_meta->is_bool() || !shadowed || !shadowed->is_array() || !flags || !flags->is_array()) {
			return std::nullopt;
		}
		const std::string_view relative = std::string_view(*path).substr(std::string_view("res://").size());
		if (*type != edt::asset_type_of(relative)) {
			return std::nullopt;
		}
		edt::asset_record record;
		record.path = res::tag{ *path };
		record.layer = static_cast<std::size_t>(*layer);
		record.size = *size;
		record.mtime = *mtime;
		record.has_meta = has_meta->get_bool();
		record.meta_size = *meta_size;
		record.meta_mtime = *meta_mtime;
		record.content_checked_at = *content_checked_at;
		record.meta_checked_at = *meta_checked_at;
		record.type = *type;
		record.content_hash = *hash;
		// Lower layers only, ascending, each once: as the walk lists them.
		for (const boost::json::value& item : shadowed->get_array()) {
			if (!item.is_int64() || item.get_int64() < 0 || static_cast<std::size_t>(item.get_int64()) >= record.layer
				|| (!record.shadowed_layers.empty() && static_cast<std::size_t>(item.get_int64()) <= record.shadowed_layers.back())) {
				return std::nullopt;
			}
			record.shadowed_layers.push_back(static_cast<std::size_t>(item.get_int64()));
		}
		if (const boost::json::value* id = object->if_contains("id")) {
			if (!id->is_string()) {
				return std::nullopt;
			}
			record.id = res::asset_id_from_text(std::string_view{ id->get_string() });
			if (!record.id) {
				return std::nullopt;
			}
		}
		if (const boost::json::value* error = object->if_contains("meta_error")) {
			if (!error->is_string()) {
				return std::nullopt;
			}
			record.meta_error = std::string{ error->get_string() };
		}
		for (const boost::json::value& flag : flags->get_array()) {
			if (!flag.is_string()) {
				return std::nullopt;
			}
			record.flags.emplace_back(flag.get_string());
		}
		if (const boost::json::value* desc_type = object->if_contains("desc_type")) {
			if (!desc_type->is_string() || record.type != "desc" || record.content_hash.empty()) {
				return std::nullopt;
			}
			record.desc_type = std::string{ desc_type->get_string() };
		}
		return record;
	}

	// A stamp within the granularity of the time it was checked may hide a change made in the same tick (a file saved
	// twice, or replaced by one of the same size and time: cp -p, rsync): what was learned from it is read again.
	void distrust_racy_stamps(edt::asset_record& record)
	{
		const std::int64_t margin = std::chrono::duration_cast<fs::file_time_type::duration>(edt::asset_database::stamp_granularity).count();
		if (record.mtime >= record.content_checked_at - margin) {
			record.content_hash.clear();
			record.desc_type.reset();
		}
		if (record.has_meta && record.meta_mtime >= record.meta_checked_at - margin) {
			// No stamp of a real file: the walk reads the .resource again.
			record.meta_size = -1;
		}
	}
}

std::optional<std::string> edt::write_cache(const asset_cache_location& cache, const std::vector<fs::path>& layers,
	const std::map<std::string, asset_record>& records)
{
	boost::json::object document;
	document["__type"] = k_cache_type;
	document["__version"] = k_cache_version;
	document["layers"] = layer_texts(layers);
	boost::json::array items;
	items.reserve(records.size());
	for (const auto& [key, record] : records) {
		items.push_back(to_json(record));
	}
	document["records"] = std::move(items);
	return prj::write_project_file(cache.root, cache.relative / "assets.json", boost::json::serialize(document));
}

void edt::asset_database::load_cache()
{
	const std::string file = fs_paths::utf8_of(cache_file());
	const auto rejected = [&file](std::string_view why) {
		egLOG_WARN("assets", "{}: {}, the asset database is built from the files", file, why);
	};
	const prj::project_file_text read = prj::read_project_file(m_cache.root, m_cache.relative / "assets.json", max_cache_bytes);
	if (!read.error.empty()) {
		rejected(read.error);
		return;
	}
	if (!read.exists) {
		return;
	}
	boost::system::error_code ec;
	const boost::json::value document = boost::json::parse(read.text, ec);
	const boost::json::object* root = ec ? nullptr : document.if_object();
	if (!root) {
		rejected("not a JSON object");
		return;
	}
	const boost::json::value* type = root->if_contains("__type");
	const boost::json::value* version = root->if_contains("__version");
	if (!type || !type->is_string() || type->get_string() != k_cache_type || !version || !version->is_int64()
		|| version->get_int64() != k_cache_version) {
		rejected("another format");
		return;
	}
	// The layers are part of what a record means (its layer index): a cache of other layers is of no use.
	const boost::json::value* layers = root->if_contains("layers");
	if (!layers || *layers != boost::json::value(layer_texts(m_layers))) {
		return;
	}
	const boost::json::value* records = root->if_contains("records");
	if (!records || !records->is_array()) {
		rejected("no records");
		return;
	}
	std::map<std::string, asset_record> loaded;
	for (const boost::json::value& item : records->get_array()) {
		std::optional<asset_record> record = record_of(item, m_layers.size());
		if (!record) {
			rejected("a record is malformed");
			return;
		}
		distrust_racy_stamps(*record);
		std::string key = record->path.string();
		if (!loaded.emplace(std::move(key), std::move(*record)).second) {
			rejected("a path is listed twice");
			return;
		}
	}
	m_cached = std::move(loaded);
	m_loaded_from_cache = true;
}

void edt::asset_database::save()
{
	if (!m_lock || !m_dirty) {
		return;
	}
	if (const std::optional<std::string> error = m_writer->write(++m_snapshots, [this] { return write_cache(m_cache, m_layers, m_records); })) {
		egLOG_WARN("assets", "the asset database cache was not written: {}", *error);
		return;
	}
	m_dirty = false;
}

namespace
{
	// The res:// path text of a tag without its protocol; none for a tag of another protocol or a path the walk never
	// makes (an empty name, "." or "..").
	std::optional<std::string> relative_of(const res::tag& tag)
	{
		if (tag.protocol() != res::tag::default_protocol()) {
			return std::nullopt;
		}
		std::string_view text = tag.view();
		text.remove_prefix(text.find("://") + 3);
		if (text.empty()) {
			return std::nullopt;
		}
		for (std::string_view rest = text;;) {
			const std::size_t slash = rest.find('/');
			const std::string_view name = rest.substr(0, slash);
			if (name.empty() || name == "." || name == "..") {
				return std::nullopt;
			}
			if (slash == std::string_view::npos) {
				break;
			}
			rest.remove_prefix(slash + 1);
		}
		return std::string{ text };
	}

	// Below a folder the walk skips: a folder on the way, or the folder itself, whose name starts with '.'.
	bool under_hidden_folder(std::string_view relative, bool is_directory)
	{
		while (true) {
			const std::size_t slash = relative.find('/');
			const bool last = slash == std::string_view::npos;
			if ((!last || is_directory) && relative.starts_with('.')) {
				return true;
			}
			if (last) {
				return false;
			}
			relative.remove_prefix(slash + 1);
		}
	}

	std::string joined(const std::string& folder, const std::string& name)
	{
		return folder.empty() ? name : folder + "/" + name;
	}

	// (folder, name) of a path text.
	std::pair<std::string, std::string> split_last(const std::string& relative)
	{
		const std::size_t slash = relative.find_last_of('/');
		if (slash == std::string::npos) {
			return { std::string{}, relative };
		}
		return { relative.substr(0, slash), relative.substr(slash + 1) };
	}
}

// The layers as walk() merges them, for one batch of changes: each folder of a layer is listed at most once.
class edt::asset_layer_lookup
{
public:
	// A folder of the merged tree: its path as the walk names it, and the layers (ascending) where it is a real folder,
	// with its path in each.
	struct folder
	{
		std::string relative;
		std::vector<std::pair<std::size_t, std::string>> layers;
	};

	// One name of a merged folder, as walk() merges the entries of the layers into a slot.
	struct slot
	{
		bool found = false;
		bool is_dir = false;
		std::size_t layer = 0;
		// The name in the winning layer, and the path there.
		std::string name;
		std::string in_layer;
		std::vector<std::size_t> shadowed;
		std::vector<std::pair<std::size_t, std::string>> dir_layers;
	};

	asset_layer_lookup(const std::vector<fs::path>& layers, bool case_insensitive)
		: m_layers(layers)
		, m_case_insensitive(case_insensitive)
	{
	}

	std::string key_of(const std::string& text) const { return edt::layer_merge_key(text, m_case_insensitive); }

	// The merged folder `relative` ("" for the root); none when the walk does not enter it.
	const std::optional<folder>& folder_at(const std::string& relative)
	{
		const std::string key = key_of(relative);
		if (const auto known = m_folders.find(key); known != m_folders.end()) {
			return known->second;
		}
		std::optional<folder> made = resolve_folder(relative);
		return m_folders.insert_or_assign(key, std::move(made)).first->second;
	}

	// The slot `name` (any letter case when case insensitive) of `in`: walk() skips what redirects and hidden folders,
	// an upper entry wins, and only the layers where all of them are folders are entered below.
	slot merge(const folder& in, const std::string& name)
	{
		slot out;
		for (const auto& [layer, path] : in.layers) {
			const std::optional<fs::directory_entry> entry = entry_of(layer, path, name);
			if (!entry || redirects(*entry)) {
				continue;
			}
			std::error_code ec;
			const bool is_dir = entry->is_directory(ec);
			if (ec) {
				continue;
			}
			std::string on_disk = fs_paths::utf8_of(entry->path().filename());
			if (is_dir && on_disk.starts_with('.')) {
				continue;
			}
			std::string child = joined(path, on_disk);
			if (out.found) {
				out.shadowed.push_back(out.layer);
			}
			if (is_dir && out.found && out.is_dir) {
				out.dir_layers.emplace_back(layer, child);
			} else if (is_dir) {
				out.dir_layers = { { layer, child } };
			} else {
				out.dir_layers.clear();
			}
			out.found = true;
			out.is_dir = is_dir;
			out.layer = layer;
			out.name = std::move(on_disk);
			out.in_layer = std::move(child);
		}
		return out;
	}

	// The .resource the walk pairs with the file `name` of `in`, in any letter case of the suffix: of the sidecar slots
	// that are files, the first by merge key (scan_asset_layers keeps the first it meets).
	slot sidecar(const folder& in, const std::string& name)
	{
		const std::string owner = key_of(name);
		std::map<std::string, std::string> names;
		for (const auto& [layer, path] : in.layers) {
			for (const listed& item : list(layer, path)) {
				if (is_sidecar(item.name) && key_of(std::string{ sidecar_owner(item.name) }) == owner) {
					names.emplace(item.key, item.name);
				}
			}
		}
		for (const auto& [key, sidecar_name] : names) {
			slot found = merge(in, sidecar_name);
			if (found.found && !found.is_dir) {
				return found;
			}
		}
		return {};
	}

private:
	struct listed
	{
		std::string name;
		std::string key;
		fs::directory_entry entry;
	};

	const std::vector<listed>& list(std::size_t layer, const std::string& path)
	{
		const auto [found, added] = m_listings.try_emplace(std::pair<std::size_t, std::string>{ layer, path });
		if (!added) {
			return found->second;
		}
		std::error_code ec;
		fs::directory_iterator it(m_layers[layer] / fs_paths::path_from_utf8(path), fs::directory_options::skip_permission_denied, ec);
		for (; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
			std::string name = fs_paths::utf8_of(it->path().filename());
			std::string key = key_of(name);
			found->second.push_back(listed{ std::move(name), std::move(key), *it });
		}
		return found->second;
	}

	// The entry of `name` in the folder `path` of `layer`; case insensitive: the one the folder lists under that key
	// (the last, as the walk's slot keeps the last).
	std::optional<fs::directory_entry> entry_of(std::size_t layer, const std::string& path, const std::string& name)
	{
		if (m_case_insensitive) {
			const std::string key = key_of(name);
			const fs::directory_entry* match = nullptr;
			for (const listed& item : list(layer, path)) {
				if (item.key == key) {
					match = &item.entry;
				}
			}
			if (!match) {
				return std::nullopt;
			}
			return *match;
		}
		std::error_code ec;
		fs::directory_entry entry(m_layers[layer] / fs_paths::path_from_utf8(joined(path, name)), ec);
		if (ec || !entry.exists(ec) || ec) {
			return std::nullopt;
		}
		return entry;
	}

	std::optional<folder> resolve_folder(const std::string& relative)
	{
		if (relative.empty()) {
			folder root;
			for (std::size_t layer = 0; layer < m_layers.size(); ++layer) {
				root.layers.emplace_back(layer, std::string{});
			}
			return root;
		}
		// walk() lists nothing in a folder of max_depth names or more.
		if (std::ranges::count(relative, '/') + 1 >= edt::asset_database::max_depth) {
			return std::nullopt;
		}
		const auto [parent, name] = split_last(relative);
		const std::optional<folder>& above = folder_at(parent);
		if (!above) {
			return std::nullopt;
		}
		slot found = merge(*above, name);
		if (!found.found || !found.is_dir) {
			return std::nullopt;
		}
		return folder{ joined(above->relative, found.name), std::move(found.dir_layers) };
	}

	const std::vector<fs::path>& m_layers;
	bool m_case_insensitive = false;
	// By merge key of the folder's path; std::map: folder_at hands out references while it adds.
	std::map<std::string, std::optional<folder>> m_folders;
	std::map<std::pair<std::size_t, std::string>, std::vector<listed>> m_listings;
};

void edt::asset_database::attach(res::resource_system& resources)
{
	if (m_resources) {
		m_resources->unsubscribe_changes(this);
	}
	m_resources = &resources;
	resources.subscribe_changes(res::tag::default_protocol(), this, [this](std::span<const res::file_change> changes) {
		apply_changes(changes);
	});
}

std::optional<edt::asset_record> edt::asset_database::look_up(const std::string& relative, asset_layer_lookup& lookup,
	const std::function<const asset_record*(const std::string&)>& before_of)
{
	// The walk makes no record of a .resource, even of one named as another .resource's file.
	if (is_sidecar(relative)) {
		return std::nullopt;
	}
	const auto [parent, name] = split_last(relative);
	const std::optional<asset_layer_lookup::folder>& folder = lookup.folder_at(parent);
	if (!folder) {
		return std::nullopt;
	}
	const asset_layer_lookup::slot file = lookup.merge(*folder, name);
	if (!file.found || file.is_dir) {
		return std::nullopt;
	}
	const std::string canonical = joined(folder->relative, file.name);
	const std::int64_t checked_at = now_ticks();
	const file_stat stat = stat_of(m_layers[file.layer] / fs_paths::path_from_utf8(file.in_layer));
	if (!stat.ok) {
		return std::nullopt;
	}
	asset_record record;
	record.content_checked_at = checked_at;
	record.meta_checked_at = checked_at;
	record.path = res::tag::make(canonical);
	record.layer = file.layer;
	record.shadowed_layers = file.shadowed;
	record.size = stat.size;
	record.mtime = stat.mtime;
	record.type = asset_type_of(canonical);
	const asset_record* before = before_of ? before_of(record.path.string()) : nullptr;
	if (before && !before->content_hash.empty() && before->size == stat.size && before->mtime == stat.mtime) {
		record.content_hash = before->content_hash;
		record.desc_type = before->desc_type;
		record.content_checked_at = std::min(record.content_checked_at, before->content_checked_at);
	}

	const asset_layer_lookup::slot meta = lookup.sidecar(*folder, file.name);
	if (!meta.found) {
		return record;
	}
	const fs::path meta_relative = fs_paths::path_from_utf8(meta.in_layer);
	const file_stat meta_stat = stat_of(m_layers[meta.layer] / meta_relative);
	record.meta_size = meta_stat.size;
	record.meta_mtime = meta_stat.mtime;
	if (before && before->has_meta && before->meta_size == meta_stat.size && before->meta_mtime == meta_stat.mtime) {
		record.has_meta = true;
		record.id = before->id;
		record.flags = before->flags;
		record.meta_error = before->meta_error;
		record.meta_checked_at = std::min(record.meta_checked_at, before->meta_checked_at);
		return record;
	}
	++m_files_read;
	const meta_read read = read_meta(m_layers[meta.layer], meta_relative);
	record.has_meta = read.present;
	if (read.meta) {
		record.id = read.meta->guid;
		record.flags = read.meta->flags;
	} else {
		record.meta_error = read.error;
	}
	return record;
}

edt::asset_batch edt::asset_database::apply_changes(std::span<const res::file_change> changes)
{
	// The walk's result would replace what this batch finds (or, before the first walk, take it for what the cache
	// knows): the batch waits for the result.
	const bool defer = m_scanning || !m_walked_once;
	const auto visible = [](const res::tag& tag, bool is_directory) -> std::optional<std::string> {
		std::optional<std::string> relative = relative_of(tag);
		if (!relative || under_hidden_folder(*relative, is_directory)) {
			return std::nullopt;
		}
		return relative;
	};
	bool walk_again = false;
	std::vector<std::string> touched;
	touched.reserve(changes.size());
	for (const res::file_change& change : changes) {
		std::optional<std::string> now = visible(change.tag, change.is_directory);
		std::optional<std::string> was = change.kind == res::change_kind::renamed ? visible(change.old_tag, change.is_directory) : std::nullopt;
		if (!now && !was) {
			continue;
		}
		if (change.is_directory) {
			walk_again = true;
			continue;
		}
		if (defer) {
			if (m_deferred.size() < max_deferred_changes) {
				m_deferred.push_back(change);
			} else {
				m_deferred.clear();
				m_walk_again = true;
			}
			continue;
		}
		if (now) {
			touched.push_back(std::move(*now));
		}
		if (was) {
			touched.push_back(std::move(*was));
		}
	}
	if (defer) {
		if (walk_again) {
			m_walk_again = true;
		}
		return {};
	}

	asset_layer_lookup lookup(m_layers, m_case_insensitive);
	// By merge key: on Windows "A.png" and "a.png" are one file.
	std::map<std::string, std::string> files;
	for (std::string& relative : touched) {
		if (is_sidecar(relative)) {
			relative.resize(sidecar_owner(relative).size());
		}
		std::string key = lookup.key_of(relative);
		files.emplace(std::move(key), std::move(relative));
	}
	// The records by merge key of their path text, to find one under another letter case.
	std::unordered_multimap<std::string, std::string> folded;
	if (m_case_insensitive && !files.empty()) {
		folded.reserve(m_records.size());
		for (const auto& [key, record] : m_records) {
			folded.emplace(lookup.key_of(key), key);
		}
	}

	asset_batch batch;
	std::vector<asset_record> gone;
	std::vector<std::string> came;
	const std::vector<asset_orphan> orphans_before = m_orphans;
	bool records_changed = false;
	for (const auto& file : files) {
		const std::string& owner_key = file.first;
		const std::string& relative = file.second;
		const std::string event_key = res::tag::make(relative).string();
		std::vector<std::string> existing;
		if (m_case_insensitive) {
			const auto [first, last] = folded.equal_range(lookup.key_of(event_key));
			for (auto it = first; it != last; ++it) {
				if (m_records.contains(it->second)) {
					existing.push_back(it->second);
				}
			}
		} else if (m_records.contains(event_key)) {
			existing.push_back(event_key);
		}
		std::optional<asset_record> now = look_up(relative, lookup, [this, &existing](const std::string& key) -> const asset_record* {
			if (const auto found = m_records.find(key); found != m_records.end()) {
				return &found->second;
			}
			// The same file under another letter case: its stamps decide what is carried over.
			return existing.empty() ? nullptr : &m_records.at(existing.front());
		});
		// A record of the file under another letter case than the one on the disk now is gone.
		for (const std::string& key : existing) {
			if (now && key == now->path.string()) {
				continue;
			}
			const auto old = m_records.find(key);
			unindex_id(old->second);
			gone.push_back(std::move(old->second));
			m_records.erase(old);
			records_changed = true;
		}
		std::erase_if(m_orphans, [&lookup, &owner_key](const asset_orphan& orphan) {
			const std::optional<std::string> meta = relative_of(orphan.meta_path);
			return meta && is_sidecar(*meta) && lookup.key_of(std::string{ sidecar_owner(*meta) }) == owner_key;
		});
		if (!now) {
			// No file the walk would list; a .resource left behind is an orphan.
			const auto [parent, name] = split_last(relative);
			const std::optional<asset_layer_lookup::folder>& folder = lookup.folder_at(parent);
			if (!folder) {
				continue;
			}
			const asset_layer_lookup::slot meta = lookup.sidecar(*folder, name);
			if (!meta.found) {
				continue;
			}
			asset_orphan orphan;
			orphan.meta_path = res::tag::make(joined(folder->relative, meta.name));
			++m_files_read;
			const meta_read read = read_meta(m_layers[meta.layer], fs_paths::path_from_utf8(meta.in_layer));
			if (read.meta) {
				orphan.id = read.meta->guid;
			}
			m_orphans.push_back(std::move(orphan));
			continue;
		}
		const std::string key = now->path.string();
		const auto known = m_records.find(key);
		const asset_record* before = known == m_records.end() ? nullptr : &known->second;
		const bool same = before && before->layer == now->layer && before->size == now->size && before->mtime == now->mtime
			&& before->meta_size == now->meta_size && before->meta_mtime == now->meta_mtime;
		if (!before) {
			came.push_back(key);
		} else if (!same) {
			batch.changed.push_back(now->path);
		}
		// A key in flight is queued again when its job ends, as the record's stamp moved (hash_next).
		if (now->content_hash.empty() && std::ranges::find(m_in_flight, key) == m_in_flight.end()
			&& std::ranges::find(m_unhashed, key) == m_unhashed.end()) {
			m_unhashed.push_back(key);
		}
		if (!before || !before->same_as(*now)) {
			records_changed = true;
		}
		if (before) {
			unindex_id(*before);
		}
		index_id(*now);
		m_records.insert_or_assign(key, std::move(*now));
	}

	// A move: what went away and what came with the same guid. The bytes moved too when the size and time did.
	// Paired only within this batch (one poll of the watcher, about 0.5 s): a move split over two polls is a removed
	// and an added, so `moved` is not complete and S57-7 must not rely on it alone.
	for (const std::string& key : came) {
		asset_record& arrived = m_records.at(key);
		const auto match = arrived.id
			? std::ranges::find_if(gone, [&arrived](const asset_record& left) { return left.id == arrived.id; })
			: gone.end();
		if (match == gone.end()) {
			batch.added.push_back(arrived.path);
			continue;
		}
		if (arrived.content_hash.empty() && !match->content_hash.empty() && match->size == arrived.size && match->mtime == arrived.mtime) {
			arrived.content_hash = match->content_hash;
			arrived.desc_type = match->desc_type;
			arrived.content_checked_at = std::min(arrived.content_checked_at, match->content_checked_at);
			std::erase(m_unhashed, key);
		}
		batch.moved.emplace_back(match->path, arrived.path);
		gone.erase(match);
	}
	for (const asset_record& left : gone) {
		batch.removed.push_back(left.path);
	}
	std::ranges::sort(m_orphans, {}, [](const asset_orphan& orphan) { return orphan.meta_path.string(); });
	if (records_changed) {
		m_dirty = true;
	}
	if (records_changed || m_orphans != orphans_before) {
		++m_revision;
	}
	if (walk_again) {
		rebuild();
	}
	hash_next();
	save_when_idle();
	if (m_on_batch) {
		m_on_batch(batch);
	}
	return batch;
}
