#include "edt_asset_database.h"
#include "edt_asset_layers.h"
#include <snk/editor/edt_job_service.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_asset_meta.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <iterator>
#include <limits>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>

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

	bool ends_with_meta_suffix(std::string_view name)
	{
		return name.size() > edt::k_asset_meta_suffix.size() && name.ends_with(edt::k_asset_meta_suffix);
	}

	struct file_stat
	{
		bool ok = false;
		std::int64_t size = 0;
		std::int64_t mtime = 0;
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

	struct merged_file
	{
		fs::path physical;
		fs::path relative;
		std::size_t layer = 0;
	};

	void walk(const std::vector<fs::path>& layers, const fs::path& relative_dir, std::vector<merged_file>& out,
		const std::function<bool()>& stopped)
	{
		for (const edt::layer_entry& entry : edt::list_merged_directory(layers, relative_dir)) {
			if (stopped && stopped()) {
				return;
			}
			std::error_code ec;
			if (fs::is_symlink(fs::symlink_status(entry.path, ec)) || ec) {
				continue;
			}
			if (entry.is_dir) {
				if (entry.name != ".git") {
					walk(layers, entry.relative, out, stopped);
				}
				continue;
			}
			out.push_back(merged_file{ entry.path, entry.relative, entry.layer });
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
	walk(layers, fs::path{}, files, stopped);

	std::unordered_map<std::string, const merged_file*> metas;
	for (const merged_file& file : files) {
		const std::string relative = file.relative.generic_string();
		if (ends_with_meta_suffix(relative)) {
			metas.emplace(relative, &file);
		}
	}

	asset_scan scan;
	scan.records.reserve(files.size() - metas.size());
	std::unordered_map<std::string, bool> has_file;
	for (const merged_file& file : files) {
		if (stopped && stopped()) {
			return scan;
		}
		const std::string relative = file.relative.generic_string();
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
		record.size = stat.size;
		record.mtime = stat.mtime;
		record.type = asset_type_of(relative);
		const std::string meta_relative = relative + std::string{ k_asset_meta_suffix };
		if (const auto meta = metas.find(meta_relative); meta != metas.end()) {
			has_file.emplace(meta_relative, true);
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

	for (const auto& [relative, file] : metas) {
		if (has_file.contains(relative)) {
			continue;
		}
		asset_orphan orphan;
		orphan.meta_path = res::tag::make(relative);
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
	platform::beneath::read_request request;
	request.root = layer_root;
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = std::numeric_limits<std::uint64_t>::max();

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
	out.ok = true;
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

edt::asset_database::asset_database(job_service& jobs, std::vector<fs::path> layers)
	: m_jobs(jobs)
{
	m_layers.reserve(layers.size());
	for (fs::path& layer : layers) {
		std::error_code ec;
		fs::path canonical = fs::weakly_canonical(layer, ec);
		m_layers.push_back(ec ? std::move(layer) : std::move(canonical));
	}
}

edt::asset_database::~asset_database() = default;

void edt::asset_database::rebuild()
{
	const std::uint64_t number = ++m_scan_number;
	m_scanning = true;
	m_jobs.start(ds::lane::io, ds::task_desc{ "Scan assets", "assets", false }, m_lifetime,
		[layers = m_layers](ds::task_context& ctx) {
			return scan_asset_layers(layers, [&ctx] { return ctx.stop.stop_requested(); });
		},
		[this, number](ds::task_result<asset_scan>& result) {
			if (number != m_scan_number) {
				return;
			}
			m_scanning = false;
			if (!result.ok()) {
				egLOG_WARN("assets", "the walk of the asset tree failed: {}", result.error);
				return;
			}
			apply_scan(std::move(*result.value));
		});
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
		if (record.content_hash.empty()) {
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
	if (m_hashing > 0 || m_unhashed.empty()) {
		return;
	}
	struct item
	{
		std::string key;
		fs::path root;
		fs::path relative;
		bool is_desc = false;
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
		batch.push_back(item{ key, m_layers[record.layer], fs::path{ std::string{ relative } }, record.type == "desc",
			record.size, record.mtime });
	}
	if (batch.empty()) {
		return;
	}
	m_hashing = batch.size();
	using results = std::vector<std::pair<item, asset_content>>;
	m_jobs.start(ds::lane::io, ds::task_desc{ "Hash assets", "assets", false }, m_lifetime,
		[batch = std::move(batch)](ds::task_context& ctx) {
			results out;
			out.reserve(batch.size());
			for (const item& entry : batch) {
				if (ctx.stop.stop_requested()) {
					break;
				}
				out.emplace_back(entry, read_asset_content(entry.root, entry.relative, entry.is_desc, max_desc_bytes));
			}
			return out;
		},
		[this](ds::task_result<results>& result) {
			m_hashing = 0;
			if (result.ok()) {
				bool changed = false;
				for (auto& [entry, content] : *result.value) {
					const auto found = m_records.find(entry.key);
					// Changed meanwhile: the next walk or batch of changes hashes it again.
					if (found == m_records.end() || found->second.size != entry.size || found->second.mtime != entry.mtime) {
						continue;
					}
					if (!content.ok) {
						egLOG_WARN("assets", "{}: {}", entry.key, content.error);
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
				egLOG_WARN("assets", "hashing assets failed: {}", result.error);
			}
			hash_next();
		});
}

std::size_t edt::asset_database::pending_hashes() const
{
	return m_unhashed.size() + m_hashing;
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
