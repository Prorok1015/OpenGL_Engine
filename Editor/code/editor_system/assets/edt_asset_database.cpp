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
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
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
#if defined(_WIN32)
	constexpr bool k_case_insensitive = true;
#else
	constexpr bool k_case_insensitive = false;
#endif

	std::uint64_t fnv1a_64_update(std::uint64_t hash, std::span<const std::byte> bytes)
	{
		for (const std::byte b : bytes) {
			hash ^= static_cast<std::uint64_t>(b);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	bool ends_with_meta_suffix(std::string_view name)
	{
		return name.size() > edt::k_asset_meta_suffix.size() && name.ends_with(edt::k_asset_meta_suffix);
	}

	struct file_stat
	{
		bool ok = false;
		std::int64_t size = 0;
		std::int64_t mtime = 0;

		bool operator==(const file_stat&) const = default;
	};

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
		// The \\?\ prefix lifts MAX_PATH for an absolute path.
		std::wstring query = entry.path().native();
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
				slot& found = merged[edt::layer_merge_key(name, k_case_insensitive)];
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
	if (has(".desc")) return "desc";
	for (const std::string_view suffix : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".ktx", ".ktx2", ".dds" }) {
		if (has(suffix)) return "texture";
	}
	for (const std::string_view suffix : { ".glb", ".gltf", ".obj", ".fbx" }) {
		if (has(suffix)) return "model";
	}
	for (const std::string_view suffix : { ".glsl", ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese", ".shader" }) {
		if (has(suffix)) return "shader";
	}
	return "file";
}

edt::asset_scan edt::scan_asset_layers(const std::vector<fs::path>& layers, const std::function<bool()>& stopped,
	const std::map<std::string, asset_record>* known)
{
	std::vector<merged_file> files;
	std::vector<std::size_t> all_layers(layers.size());
	for (std::size_t i = 0; i < layers.size(); ++i) {
		all_layers[i] = i;
	}
	walk_state state{ layers, stopped, files, false };
	walk(state, fs::path{}, all_layers, 0);

	// Keyed as the merge keys names: on Windows "A.png.resource" belongs to "a.png".
	std::unordered_map<std::string, const merged_file*> metas;
	for (const merged_file& file : files) {
		const std::string relative = fs_paths::generic_utf8_of(file.relative);
		if (ends_with_meta_suffix(relative)) {
			metas.emplace(edt::layer_merge_key(relative, k_case_insensitive), &file);
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
		if (ends_with_meta_suffix(relative)) {
			continue;
		}
		const file_stat stat = stat_of(file.physical);
		if (!stat.ok) {
			continue;
		}
		asset_record record;
		record.path = res::tag::make(relative);
		record.layer = file.layer;
		record.shadowed_layers = file.shadowed;
		record.size = stat.size;
		record.mtime = stat.mtime;
		record.type = asset_type_of(relative);
		const std::string meta_key = edt::layer_merge_key(relative + std::string{ k_asset_meta_suffix }, k_case_insensitive);
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

edt::asset_database::asset_database(job_service& jobs, editor_events* events, std::vector<fs::path> layers, fs::path cache_dir)
	: m_jobs(jobs)
	, m_cache_dir(std::move(cache_dir))
{
	m_layers.reserve(layers.size());
	for (fs::path& layer : layers) {
		std::error_code ec;
		fs::path canonical = fs::weakly_canonical(layer, ec);
		m_layers.push_back(ec ? std::move(layer) : std::move(canonical));
	}
	if (!m_cache_dir.empty()) {
		std::string error;
		m_lock = platform::file_lock::try_acquire(m_cache_dir / ".lock", &error);
		if (!m_lock) {
			egLOG_INFO("assets", "{}: the asset database cache is held by another editor{}; this one keeps its database in memory",
				fs_paths::utf8_of(m_cache_dir), error.empty() ? std::string{} : " (" + error + ")");
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
	save();
}

void edt::asset_database::rebuild()
{
	if (m_scanning && m_scan_job != 0) {
		m_jobs.cancel(m_scan_job);
	}
	// A hashing job someone cancelled without the database seeing it (no events): its keys go back into the queue.
	if (m_hash_job != 0 && !m_jobs.is_running(m_hash_job)) {
		requeue_in_flight();
	}
	const std::uint64_t number = ++m_scan_number;
	m_scanning = true;
	m_scan_job = m_jobs.start(ds::lane::io, ds::task_desc{ "Scan assets", "assets", true }, m_lifetime,
		[layers = m_layers, known = m_records](ds::task_context& ctx) {
			return scan_asset_layers(layers, [&ctx] { return ctx.stop.stop_requested(); }, &known);
		},
		[this, number](ds::task_result<asset_scan>& result) {
			if (number != m_scan_number) {
				return;
			}
			m_scanning = false;
			m_scan_job = 0;
			if (!result.ok()) {
				egLOG_WARN("assets", "the walk of the asset tree failed: {}", result.error);
				return;
			}
			apply_scan(std::move(*result.value));
		});
}

void edt::asset_database::on_job_finished(std::uint64_t id, job_status status)
{
	if (status != job_status::cancelled) {
		return;
	}
	if (id != 0 && id == m_scan_job) {
		m_scanning = false;
		m_scan_job = 0;
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
	std::map<std::string, asset_record> previous = std::move(m_records);
	m_records.clear();
	m_by_id.clear();
	m_unhashed.clear();
	for (asset_record& record : scan.records) {
		std::string key = record.path.string();
		// A file the walk found as it was keeps what the background pass learned of it.
		if (const auto known = previous.find(key);
			known != previous.end() && known->second.size == record.size && known->second.mtime == record.mtime) {
			record.content_hash = known->second.content_hash;
			record.desc_type = known->second.desc_type;
		}
		const bool in_flight = std::ranges::find(m_in_flight, key) != m_in_flight.end();
		if (record.content_hash.empty() && !in_flight) {
			m_unhashed.push_back(key);
		}
		index_id(record);
		m_records.emplace(std::move(key), std::move(record));
	}
	m_orphans = std::move(scan.orphans);
	m_files_read += scan.meta_reads;
	m_dirty = true;
	++m_revision;
	hash_next();
	save_when_idle();
}

void edt::asset_database::index_id(const asset_record& record)
{
	if (record.id) {
		m_by_id.emplace(*record.id, record.path.string());
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
		batch.push_back(item{ std::move(key), m_layers[record.layer], fs_paths::path_from_utf8(relative), record.type == "desc" });
	}
	if (batch.empty()) {
		return;
	}
	using results = std::vector<std::pair<std::string, asset_content>>;
	m_hash_job = m_jobs.start(ds::lane::io, ds::task_desc{ "Hash assets", "assets", true }, m_lifetime,
		[batch = std::move(batch)](ds::task_context& ctx) {
			results out;
			out.reserve(batch.size());
			for (const item& entry : batch) {
				if (ctx.stop.stop_requested()) {
					break;
				}
				out.emplace_back(entry.key, read_asset_content(entry.root, entry.relative, entry.is_desc, max_desc_bytes));
			}
			return out;
		},
		[this](ds::task_result<results>& result) {
			if (result.ok()) {
				bool changed = false;
				for (auto& [key, content] : *result.value) {
					std::erase(m_in_flight, key);
					++m_files_read;
					const auto found = m_records.find(key);
					if (found == m_records.end()) {
						continue;
					}
					if (!content.ok) {
						egLOG_WARN("assets", "{}: {}", key, content.error);
						continue;
					}
					// Changed since the walk: the next walk or batch of changes updates the record and hashes it again.
					if (found->second.size != content.size || found->second.mtime != content.mtime) {
						continue;
					}
					// Written while it was read, now as the record says: hashed again with the next batch.
					if (!content.unchanged) {
						m_unhashed.push_back(key);
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
	if (!m_scanning && m_hash_job == 0 && m_unhashed.empty()) {
		save();
	}
}

namespace
{
	constexpr std::string_view k_cache_type = "asset_db";
	constexpr std::int64_t k_cache_version = 1;

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
		return out;
	}

	std::optional<edt::asset_record> record_of(const boost::json::value& value, std::size_t layer_count)
	{
		const boost::json::object* object = value.if_object();
		if (!object) {
			return std::nullopt;
		}
		const auto text = [object](std::string_view key) -> std::optional<std::string> {
			const boost::json::value* field = object->if_contains(key);
			if (!field || !field->is_string()) return std::nullopt;
			return std::string{ field->get_string() };
		};
		const auto integer = [object](std::string_view key) -> std::optional<std::int64_t> {
			const boost::json::value* field = object->if_contains(key);
			if (!field) return std::nullopt;
			if (field->is_int64()) return field->get_int64();
			if (field->is_uint64()) return static_cast<std::int64_t>(field->get_uint64());
			return std::nullopt;
		};
		const auto path = text("path");
		const auto layer = integer("layer");
		const auto size = integer("size");
		const auto mtime = integer("mtime");
		const auto type = text("type");
		const auto hash = text("hash");
		if (!path || !layer || *layer < 0 || static_cast<std::size_t>(*layer) >= layer_count || !size || !mtime || !type || !hash) {
			return std::nullopt;
		}
		edt::asset_record record;
		record.path = res::tag{ *path };
		record.layer = static_cast<std::size_t>(*layer);
		record.size = *size;
		record.mtime = *mtime;
		record.type = *type;
		record.content_hash = *hash;
		if (const boost::json::value* shadowed = object->if_contains("shadowed"); shadowed && shadowed->is_array()) {
			for (const boost::json::value& item : shadowed->get_array()) {
				if (item.is_int64() && item.get_int64() >= 0) record.shadowed_layers.push_back(static_cast<std::size_t>(item.get_int64()));
				else if (item.is_uint64()) record.shadowed_layers.push_back(static_cast<std::size_t>(item.get_uint64()));
			}
		}
		if (const auto id = text("id")) {
			record.id = res::asset_id_from_text(*id);
			if (!record.id) return std::nullopt;
		}
		if (const boost::json::value* has_meta = object->if_contains("has_meta"); has_meta && has_meta->is_bool()) {
			record.has_meta = has_meta->get_bool();
		}
		record.meta_size = integer("meta_size").value_or(0);
		record.meta_mtime = integer("meta_mtime").value_or(0);
		record.meta_error = text("meta_error").value_or(std::string{});
		if (const boost::json::value* flags = object->if_contains("flags"); flags && flags->is_array()) {
			for (const boost::json::value& flag : flags->get_array()) {
				if (flag.is_string()) record.flags.emplace_back(flag.get_string());
			}
		}
		record.desc_type = text("desc_type");
		return record;
	}
}

void edt::asset_database::load_cache()
{
	const fs::path file = m_cache_dir / "assets.json";
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		return;
	}
	std::ostringstream text;
	text << in.rdbuf();
	boost::system::error_code ec;
	const boost::json::value document = boost::json::parse(text.str(), ec);
	const boost::json::object* root = ec ? nullptr : document.if_object();
	const auto rejected = [&file](std::string_view why) {
		egLOG_WARN("assets", "{}: {}, the asset database is built from the files", fs_paths::utf8_of(file), why);
	};
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
		std::string key = record->path.string();
		loaded.emplace(std::move(key), std::move(*record));
	}
	m_records = std::move(loaded);
	for (const auto& [key, record] : m_records) {
		index_id(record);
	}
	m_loaded_from_cache = true;
	++m_revision;
}

void edt::asset_database::save()
{
	if (!m_lock || !m_dirty) {
		return;
	}
	boost::json::object document;
	document["__type"] = k_cache_type;
	document["__version"] = k_cache_version;
	document["layers"] = layer_texts(m_layers);
	boost::json::array records;
	records.reserve(m_records.size());
	for (const auto& [key, record] : m_records) {
		records.push_back(to_json(record));
	}
	document["records"] = std::move(records);
	std::error_code ec;
	fs::create_directories(m_cache_dir, ec);
	if (const std::optional<std::string> error = prj::write_text_atomically(m_cache_dir / "assets.json", boost::json::serialize(document))) {
		egLOG_WARN("assets", "the asset database cache was not written: {}", *error);
		return;
	}
	m_dirty = false;
}

namespace
{
	// The res:// path text of a tag without its protocol; none for a tag of another protocol.
	std::optional<std::string> relative_of(const res::tag& tag)
	{
		if (tag.protocol() != res::tag::default_protocol()) {
			return std::nullopt;
		}
		std::string_view text = tag.view();
		text.remove_prefix(text.find("://") + 3);
		return std::string{ text };
	}

	enum class presence { missing, file, other };

	// What `relative` is in one layer, every folder on the way checked as the walk checks entries: a link, a redirecting
	// reparse point or a hidden folder on the way means the layer does not have it.
	presence presence_in(const fs::path& layer, const std::string& relative)
	{
		fs::path at = layer;
		const fs::path rel = fs_paths::path_from_utf8(relative);
		std::size_t left = static_cast<std::size_t>(std::distance(rel.begin(), rel.end()));
		for (const fs::path& part : rel) {
			at /= part;
			--left;
			std::error_code ec;
			const fs::directory_entry entry(at, ec);
			if (ec || !entry.exists(ec)) {
				return presence::missing;
			}
			if (redirects(entry)) {
				return presence::other;
			}
			const bool is_dir = entry.is_directory(ec);
			if (left > 0) {
				if (!is_dir || fs_paths::utf8_of(part).starts_with('.')) {
					return presence::other;
				}
				continue;
			}
			return entry.is_regular_file(ec) && !ec ? presence::file : presence::other;
		}
		return presence::missing;
	}
}

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

std::optional<edt::asset_record> edt::asset_database::look_up(const std::string& relative, const asset_record* before)
{
	std::optional<std::size_t> winner;
	std::vector<std::size_t> shadowed;
	for (std::size_t layer = m_layers.size(); layer-- > 0;) {
		const presence here = presence_in(m_layers[layer], relative);
		if (here == presence::missing) {
			continue;
		}
		if (!winner) {
			if (here != presence::file) {
				return std::nullopt;
			}
			winner = layer;
		} else {
			shadowed.push_back(layer);
		}
	}
	if (!winner) {
		return std::nullopt;
	}
	std::ranges::reverse(shadowed);
	const fs::path physical = m_layers[*winner] / fs_paths::path_from_utf8(relative);
	const file_stat stat = stat_of(physical);
	if (!stat.ok) {
		return std::nullopt;
	}
	asset_record record;
	record.path = res::tag::make(relative);
	record.layer = *winner;
	record.shadowed_layers = std::move(shadowed);
	record.size = stat.size;
	record.mtime = stat.mtime;
	record.type = asset_type_of(relative);
	if (before && before->size == stat.size && before->mtime == stat.mtime) {
		record.content_hash = before->content_hash;
		record.desc_type = before->desc_type;
	}

	const std::string meta_relative = relative + std::string{ k_asset_meta_suffix };
	for (std::size_t layer = m_layers.size(); layer-- > 0;) {
		const presence here = presence_in(m_layers[layer], meta_relative);
		if (here == presence::missing) {
			continue;
		}
		if (here == presence::file) {
			const file_stat meta_stat = stat_of(m_layers[layer] / fs_paths::path_from_utf8(meta_relative));
			record.meta_size = meta_stat.size;
			record.meta_mtime = meta_stat.mtime;
			if (before && before->has_meta && before->meta_size == meta_stat.size && before->meta_mtime == meta_stat.mtime) {
				record.has_meta = true;
				record.id = before->id;
				record.flags = before->flags;
				record.meta_error = before->meta_error;
			} else {
				++m_files_read;
				const meta_read read = read_meta(m_layers[layer], fs_paths::path_from_utf8(meta_relative));
				record.has_meta = read.present;
				if (read.meta) {
					record.id = read.meta->guid;
					record.flags = read.meta->flags;
				} else {
					record.meta_error = read.error;
				}
			}
		}
		break;
	}
	return record;
}

void edt::asset_database::reindex()
{
	m_by_id.clear();
	for (const auto& [key, record] : m_records) {
		index_id(record);
	}
}

edt::asset_batch edt::asset_database::apply_changes(std::span<const res::file_change> changes)
{
	asset_batch batch;
	bool walk_again = false;
	std::set<std::string> files;
	const auto touch = [&files](const res::tag& tag) {
		std::optional<std::string> relative = relative_of(tag);
		if (!relative || relative->empty()) {
			return;
		}
		if (ends_with_meta_suffix(*relative)) {
			relative->resize(relative->size() - k_asset_meta_suffix.size());
		}
		files.insert(std::move(*relative));
	};
	for (const res::file_change& change : changes) {
		if (change.is_directory) {
			walk_again = true;
			continue;
		}
		touch(change.tag);
		if (change.kind == res::change_kind::renamed) {
			touch(change.old_tag);
		}
	}

	std::vector<asset_record> gone;
	std::vector<std::string> came;
	for (const std::string& relative : files) {
		const std::string key = res::tag::make(relative).string();
		const auto known = m_records.find(key);
		const asset_record* before = known == m_records.end() ? nullptr : &known->second;
		std::optional<asset_record> now = look_up(relative, before);
		const std::string meta_key = res::tag::make(relative + std::string{ k_asset_meta_suffix }).string();
		std::erase_if(m_orphans, [&meta_key](const asset_orphan& orphan) { return orphan.meta_path.string() == meta_key; });
		if (!now) {
			// The file is gone; a .resource left behind is an orphan.
			for (std::size_t layer = m_layers.size(); layer-- > 0;) {
				const std::string meta_relative = relative + std::string{ k_asset_meta_suffix };
				if (presence_in(m_layers[layer], meta_relative) == presence::file) {
					asset_orphan orphan;
					orphan.meta_path = res::tag::make(meta_relative);
					++m_files_read;
					const meta_read read = read_meta(m_layers[layer], fs_paths::path_from_utf8(meta_relative));
					if (read.meta) {
						orphan.id = read.meta->guid;
					}
					m_orphans.push_back(std::move(orphan));
					break;
				}
			}
			if (before) {
				gone.push_back(std::move(known->second));
				m_records.erase(known);
			}
			continue;
		}
		const bool is_new = before == nullptr;
		const bool same = before && before->layer == now->layer && before->size == now->size && before->mtime == now->mtime
			&& before->meta_size == now->meta_size && before->meta_mtime == now->meta_mtime;
		if (is_new) {
			came.push_back(key);
		} else if (!same) {
			batch.changed.push_back(now->path);
		}
		if (now->content_hash.empty() && std::ranges::find(m_in_flight, key) == m_in_flight.end()
			&& std::ranges::find(m_unhashed, key) == m_unhashed.end()) {
			m_unhashed.push_back(key);
		}
		m_records.insert_or_assign(key, std::move(*now));
	}

	// A move: what went away and what came with the same guid. The bytes moved too when the size and time did.
	for (const std::string& key : came) {
		asset_record& arrived = m_records.at(key);
		const auto match = arrived.id
			? std::ranges::find_if(gone, [&arrived](const asset_record& left) { return left.id == arrived.id; })
			: gone.end();
		if (match == gone.end()) {
			batch.added.push_back(arrived.path);
			continue;
		}
		if (arrived.content_hash.empty() && match->size == arrived.size && match->mtime == arrived.mtime) {
			arrived.content_hash = match->content_hash;
			arrived.desc_type = match->desc_type;
			std::erase(m_unhashed, key);
		}
		batch.moved.emplace_back(match->path, arrived.path);
		gone.erase(match);
	}
	for (const asset_record& left : gone) {
		batch.removed.push_back(left.path);
	}
	std::ranges::sort(m_orphans, {}, [](const asset_orphan& orphan) { return orphan.meta_path.string(); });
	reindex();
	m_dirty = true;
	++m_revision;
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
