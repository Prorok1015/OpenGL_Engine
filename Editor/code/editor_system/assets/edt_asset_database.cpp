#include "edt_asset_database.h"
#include "edt_asset_layers.h"
#include <snk/editor/edt_job_service.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_asset_meta.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <limits>
#include <map>
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
		const HANDLE found = ::FindFirstFileW(entry.path().c_str(), &data);
		if (found == INVALID_HANDLE_VALUE) {
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
		if (depth > edt::asset_database::max_depth) {
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

edt::asset_scan edt::scan_asset_layers(const std::vector<fs::path>& layers, const std::function<bool()>& stopped)
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

edt::asset_database::asset_database(job_service& jobs, editor_events* events, std::vector<fs::path> layers)
	: m_jobs(jobs)
{
	m_layers.reserve(layers.size());
	for (fs::path& layer : layers) {
		std::error_code ec;
		fs::path canonical = fs::weakly_canonical(layer, ec);
		m_layers.push_back(ec ? std::move(layer) : std::move(canonical));
	}
	if (events) {
		m_job_finished = events->job_finished.subscribe(
			[this](std::uint64_t id, job_status status, const std::string&, const std::optional<boost::json::value>&) {
				on_job_finished(id, status);
			});
	}
}

edt::asset_database::~asset_database() = default;

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
		[layers = m_layers](ds::task_context& ctx) {
			return scan_asset_layers(layers, [&ctx] { return ctx.stop.stop_requested(); });
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
	++m_revision;
	hash_next();
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
					const auto found = m_records.find(key);
					if (found == m_records.end()) {
						continue;
					}
					if (!content.ok) {
						egLOG_WARN("assets", "{}: {}", key, content.error);
						continue;
					}
					// Written while it was read, or since the walk: the next walk or batch of changes hashes it again.
					if (!content.unchanged || found->second.size != content.size || found->second.mtime != content.mtime) {
						continue;
					}
					found->second.content_hash = std::move(content.content_hash);
					found->second.desc_type = std::move(content.desc_type);
					changed = true;
				}
				if (changed) {
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
