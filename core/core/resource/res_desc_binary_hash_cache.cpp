#include <snk/resource/res_desc_binary_hash_cache.h>
#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <boost/json.hpp>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_project.h>

namespace
{
	namespace json = boost::json;

	constexpr std::string_view k_file_name = "desc_binary_verified.json";

	bool is_sha256_hex(std::string_view text)
	{
		return text.size() == 64
			&& std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}

	std::optional<std::uint64_t> unsigned_of(const json::value& value)
	{
		if (value.is_uint64()) {
			return value.get_uint64();
		}
		if (value.is_int64() && value.get_int64() >= 0) {
			return static_cast<std::uint64_t>(value.get_int64());
		}
		return std::nullopt;
	}

	std::optional<std::int64_t> signed_of(const json::value& value)
	{
		if (value.is_int64()) {
			return value.get_int64();
		}
		return std::nullopt;
	}

	// "<prefix>_size", "_mtime", "_ctime", "_id", "_dev" of `entry`, or nullopt when one is missing or of another type.
	std::optional<platform::beneath::file_stamp> stamp_of(const json::object& entry, std::string_view prefix)
	{
		const auto field = [&](std::string_view name) { return entry.if_contains(std::string(prefix) + "_" + std::string(name)); };
		const json::value* size = field("size");
		const json::value* mtime = field("mtime");
		const json::value* ctime = field("ctime");
		const json::value* id = field("id");
		const json::value* dev = field("dev");
		if (!size || !mtime || !ctime || !id || !dev) {
			return std::nullopt;
		}
		const auto bytes = unsigned_of(*size);
		const auto written = signed_of(*mtime);
		const auto changed = signed_of(*ctime);
		const auto file_id = unsigned_of(*id);
		const auto device = unsigned_of(*dev);
		if (!bytes || !written || !changed || !file_id || !device) {
			return std::nullopt;
		}
		return platform::beneath::file_stamp{ *bytes, *written, *changed, *file_id, *device };
	}

	// The entry, or nullopt when anything about it is off: a field missing, unknown or of another type.
	std::optional<res::desc_binary_pair_key> entry_of(const json::value& value)
	{
		const json::object* entry = value.if_object();
		if (entry == nullptr || entry->size() != 12) {
			return std::nullopt;
		}
		const json::value* bin = entry->if_contains("bin");
		const json::value* ranges = entry->if_contains("ranges");
		if (!bin || !ranges || !bin->is_string() || bin->get_string().empty() || !ranges->is_string()
			|| !is_sha256_hex(ranges->get_string())) {
			return std::nullopt;
		}
		const auto binary_stamp = stamp_of(*entry, "bin");
		const auto desc_stamp = stamp_of(*entry, "desc");
		if (!binary_stamp || !desc_stamp) {
			return std::nullopt;
		}
		res::desc_binary_pair_key key;
		key.binary = std::string(bin->get_string());
		key.binary_stamp = *binary_stamp;
		key.desc_stamp = *desc_stamp;
		key.ranges_sha256 = std::string(ranges->get_string());
		return key;
	}
}

std::string res::desc_binary_ranges_sha256(std::span<const binary_range> ranges)
{
	std::string text;
	for (const binary_range& range : ranges) {
		text += std::format("{} {} {} {}\n", range.offset, range.size, range.layout, range.sha256);
	}
	return cmn::sha256_hex(std::as_bytes(std::span(text)));
}

std::filesystem::path res::file_desc_binary_hash_cache::file_path()
{
	return std::filesystem::path(prj::k_cache_dir_name) / k_file_name;
}

res::file_desc_binary_hash_cache::file_desc_binary_hash_cache(std::filesystem::path project_root, std::uint64_t file_limit)
	: m_project_root(std::move(project_root))
{
	load(file_limit);
}

res::file_desc_binary_hash_cache::~file_desc_binary_hash_cache()
{
	try {
		flush();
	} catch (...) {
	}
}

void res::file_desc_binary_hash_cache::load(std::uint64_t file_limit)
{
	const prj::project_file_text read = prj::read_project_file(m_project_root, file_path(), file_limit);
	if (!read.exists) {
		return;
	}
	const auto ignore = [&](std::string_view reason) {
		egLOG_WARN("resource/desc_binary", "'{}' is ignored and will be written anew (it only spares hashing): {}",
			(m_project_root / file_path()).generic_string(), reason);
		m_entries.clear();
		m_by_binary.clear();
		m_dirty = true;
	};
	if (!read.error.empty()) {
		ignore(read.error);
		return;
	}
	json::error_code ec;
	const json::value parsed = json::parse(read.text, ec);
	if (ec) {
		ignore(std::format("not JSON: {}", ec.message()));
		return;
	}
	const json::object* top = parsed.if_object();
	const json::value* version = top ? top->if_contains("version") : nullptr;
	const json::value* entries = top ? top->if_contains("entries") : nullptr;
	if (top == nullptr || top->size() != 2 || version == nullptr || entries == nullptr) {
		ignore("expected an object of \"version\" and \"entries\" only");
		return;
	}
	if (signed_of(*version) != format_version) {
		ignore(std::format("not version {}", format_version));
		return;
	}
	const json::array* list = entries->if_array();
	if (list == nullptr || list->size() > max_entries) {
		ignore(std::format("\"entries\" is not an array of at most {} entries", max_entries));
		return;
	}
	std::lock_guard lock(m_mutex);
	for (std::size_t i = 0; i < list->size(); ++i) {
		auto key = entry_of((*list)[i]);
		if (!key) {
			ignore(std::format("entry {} is not as expected", i));
			return;
		}
		if (const auto found = m_by_binary.find(key->binary); found != m_by_binary.end()) {
			m_entries.erase(found->second);
			m_by_binary.erase(found);
		}
		std::string binary = key->binary;
		m_entries.push_back(std::move(*key));
		m_by_binary.emplace(std::move(binary), std::prev(m_entries.end()));
	}
}

bool res::file_desc_binary_hash_cache::verified(const desc_binary_pair_key& key) const
{
	std::lock_guard lock(m_mutex);
	const auto found = m_by_binary.find(key.binary);
	return found != m_by_binary.end() && *found->second == key;
}

void res::file_desc_binary_hash_cache::remember(const desc_binary_pair_key& key)
{
	std::lock_guard lock(m_mutex);
	if (const auto found = m_by_binary.find(key.binary); found != m_by_binary.end()) {
		if (*found->second == key) {
			return;
		}
		m_entries.erase(found->second);
		m_by_binary.erase(found);
	}
	while (m_entries.size() >= max_entries) {
		m_by_binary.erase(m_entries.front().binary);
		m_entries.pop_front();
	}
	m_entries.push_back(key);
	m_by_binary.emplace(key.binary, std::prev(m_entries.end()));
	m_dirty = true;
}

bool res::file_desc_binary_hash_cache::flush()
{
	std::lock_guard write_lock(m_write_mutex);
	std::string text;
	{
		std::lock_guard lock(m_mutex);
		if (!m_dirty) {
			return true;
		}
		json::array entries;
		entries.reserve(m_entries.size());
		for (const desc_binary_pair_key& key : m_entries) {
			entries.push_back(json::object{
				{ "bin", key.binary },
				{ "bin_size", key.binary_stamp.size },
				{ "bin_mtime", key.binary_stamp.write_time_ns },
				{ "bin_ctime", key.binary_stamp.change_time_ns },
				{ "bin_id", key.binary_stamp.file_id },
				{ "bin_dev", key.binary_stamp.device },
				{ "desc_size", key.desc_stamp.size },
				{ "desc_mtime", key.desc_stamp.write_time_ns },
				{ "desc_ctime", key.desc_stamp.change_time_ns },
				{ "desc_id", key.desc_stamp.file_id },
				{ "desc_dev", key.desc_stamp.device },
				{ "ranges", key.ranges_sha256 },
			});
		}
		text = json::serialize(json::object{ { "version", format_version }, { "entries", std::move(entries) } });
		m_dirty = false;
	}
	if (const auto error = prj::write_project_file(m_project_root, file_path(), text)) {
		egLOG_WARN("resource/desc_binary", "The cache of checked binary files was not written: {}", *error);
		return false;
	}
	return true;
}

bool res::file_desc_binary_hash_cache::dirty() const
{
	std::lock_guard lock(m_mutex);
	return m_dirty;
}

std::size_t res::file_desc_binary_hash_cache::size() const
{
	std::lock_guard lock(m_mutex);
	return m_entries.size();
}
