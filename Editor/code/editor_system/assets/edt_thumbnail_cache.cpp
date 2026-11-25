#include "edt_thumbnail_cache.h"
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <utility>

namespace {

namespace beneath = platform::beneath;

constexpr std::string_view k_magic = "SNKT";
constexpr std::uint32_t k_format_version = 1;
constexpr std::size_t k_key_length = 64;
constexpr std::size_t k_header_bytes = 4 + 4 + k_key_length + 4 + 4;
constexpr std::string_view k_folder = "thumbnails";
constexpr std::string_view k_suffix = ".thumb";
constexpr std::int64_t k_stale_tmp_seconds = 3600;
constexpr std::size_t k_list_limit = 16384;

void put_u32(std::vector<std::byte>& out, std::uint32_t value)
{
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
	}
}

std::uint32_t get_u32(const std::byte* at)
{
	std::uint32_t value = 0;
	for (int i = 0; i < 4; ++i) {
		value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(at[i])) << (8 * i);
	}
	return value;
}

bool is_hex_key(std::string_view text)
{
	return text.size() == k_key_length && std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool is_tmp_name(const std::string& name)
{
	return name.ends_with(".tmp") && name.size() > k_key_length && is_hex_key(std::string_view(name).substr(0, k_key_length));
}

std::int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

edt::thumbnail_disk_cache::thumbnail_disk_cache(std::filesystem::path cache_root)
	: thumbnail_disk_cache(std::move(cache_root), limits{})
{
}

edt::thumbnail_disk_cache::thumbnail_disk_cache(std::filesystem::path cache_root, limits with)
	: m_limits(with)
{
	std::error_code ec;
	m_root = std::filesystem::weakly_canonical(cache_root, ec);
	if (ec) {
		m_root = std::move(cache_root);
	}
}

std::string edt::thumbnail_disk_cache::key_of(const std::filesystem::path& relative, const beneath::file_stamp& stamp)
{
	const std::u8string path = relative.generic_u8string();
	const std::string text = std::format("snk-thumb|{}|{}|{}|{}|{}|{}|{}|{}", k_format_version, k_thumbnail_side,
		std::string(path.begin(), path.end()), stamp.size, stamp.write_time_ns, stamp.change_time_ns, stamp.file_id, stamp.device);
	return cmn::sha256_hex(std::as_bytes(std::span(text)));
}

bool edt::thumbnail_disk_cache::is_entry_name(const std::string& name)
{
	return name.size() == k_key_length + k_suffix.size() && name.ends_with(k_suffix) && is_hex_key(std::string_view(name).substr(0, k_key_length));
}

std::vector<std::byte> edt::thumbnail_disk_cache::encode_entry(const std::string& key, const thumbnail_pixels& pixels)
{
	std::vector<std::byte> out;
	out.reserve(k_header_bytes + pixels.rgba.size());
	for (const char c : k_magic) out.push_back(static_cast<std::byte>(c));
	put_u32(out, k_format_version);
	for (const char c : key) out.push_back(static_cast<std::byte>(c));
	put_u32(out, static_cast<std::uint32_t>(pixels.width));
	put_u32(out, static_cast<std::uint32_t>(pixels.height));
	for (const unsigned char c : pixels.rgba) out.push_back(static_cast<std::byte>(c));
	return out;
}

std::optional<edt::thumbnail_pixels> edt::thumbnail_disk_cache::decode_entry(const std::string& key, std::span<const std::byte> bytes)
{
	if (key.size() != k_key_length || bytes.size() < k_header_bytes) return std::nullopt;
	if (std::memcmp(bytes.data(), k_magic.data(), k_magic.size()) != 0) return std::nullopt;
	if (get_u32(bytes.data() + 4) != k_format_version) return std::nullopt;
	if (std::memcmp(bytes.data() + 8, key.data(), k_key_length) != 0) return std::nullopt;
	const std::uint32_t width = get_u32(bytes.data() + 8 + k_key_length);
	const std::uint32_t height = get_u32(bytes.data() + 12 + k_key_length);
	if (width < 1 || height < 1 || width > static_cast<std::uint32_t>(k_thumbnail_side) || height > static_cast<std::uint32_t>(k_thumbnail_side)) {
		return std::nullopt;
	}
	const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4;
	if (bytes.size() != k_header_bytes + pixel_bytes) return std::nullopt;
	thumbnail_pixels pixels;
	pixels.width = static_cast<int>(width);
	pixels.height = static_cast<int>(height);
	pixels.rgba.resize(pixel_bytes);
	std::memcpy(pixels.rgba.data(), bytes.data() + k_header_bytes, pixel_bytes);
	return pixels;
}

std::optional<edt::thumbnail_pixels> edt::thumbnail_disk_cache::read_entry(const std::string& key) const
{
	beneath::read_request request;
	request.root = m_root;
	request.folder = std::filesystem::path(k_folder);
	request.leaf = std::filesystem::path(key + std::string(k_suffix));
	request.max_bytes = k_max_entry_bytes;
	std::vector<std::byte> bytes;
	if (beneath::read_beneath(request, bytes).status != beneath::read_status::ok) return std::nullopt;
	return decode_entry(key, bytes);
}

void edt::thumbnail_disk_cache::write_entry(const std::string& key, const thumbnail_pixels& pixels) const
{
	const std::vector<std::byte> bytes = encode_entry(key, pixels);
	beneath::pinned_root root(m_root);
	const std::filesystem::path folder(k_folder);
	if (const std::string failure = beneath::make_folders_beneath(root, folder); !failure.empty()) {
		egLOG_DEBUG("thumbnail", "cache folder not made: {}", failure);
		return;
	}
	beneath::write_request request;
	request.folder = folder;
	request.leaf = std::filesystem::path(key + std::string(k_suffix));
	request.tmp_leaf = std::filesystem::path(std::format("{}.{}.tmp", key, ++m_tmp_serial));
	request.data = bytes;
	request.durable = false;
	request.replace = true;
	request.tmp_path = root.path() / folder / request.tmp_leaf;
	if (const std::string failure = beneath::write_beneath(root, request); !failure.empty()) {
		egLOG_DEBUG("thumbnail", "cache entry not written: {}", failure);
		return;
	}
	++m_writes_total;
	++m_writes_since_prune;
}

edt::thumbnail_decoded edt::thumbnail_disk_cache::load_or_decode(const std::filesystem::path& root,
	const std::filesystem::path& relative, const thumbnail_decode_fn& decode) const
{
	beneath::read_request look;
	look.root = root;
	look.folder = relative.parent_path();
	look.leaf = relative.filename();
	look.max_bytes = k_thumbnail_max_file_bytes;
	look.stamp_only = true;
	std::vector<std::byte> nothing;
	const beneath::read_result seen = beneath::read_beneath(look, nothing);
	std::string key;
	if (seen.status == beneath::read_status::ok && seen.stamp) {
		key = key_of(relative, *seen.stamp);
		if (auto pixels = read_entry(key)) {
			++m_hits;
			return thumbnail_decoded{ std::move(pixels), {} };
		}
	}

	std::optional<beneath::file_stamp> read_stamp;
	thumbnail_decoded decoded = decode(root, relative, read_stamp);
	if (decoded.pixels && !key.empty() && read_stamp && *read_stamp == *seen.stamp) {
		write_entry(key, *decoded.pixels);
	}
	return decoded;
}

bool edt::thumbnail_disk_cache::prune_due() const
{
	return !m_pruned_once || m_writes_since_prune >= m_limits.prune_every_writes;
}

std::size_t edt::thumbnail_disk_cache::prune() const
{
	m_pruned_once = true;
	m_writes_since_prune = 0;
	beneath::pinned_root root(m_root);
	const std::filesystem::path folder(k_folder);
	const beneath::list_result listing = beneath::list_beneath(root, folder, k_list_limit);
	if (listing.missing || !listing.error.empty()) return 0;

	struct kept { std::string name; std::int64_t written; std::uint64_t size; };
	std::vector<kept> entries;
	std::size_t removed = 0;
	const auto remove = [&](const std::string& name) {
		if (beneath::remove_beneath(root, folder, std::filesystem::path(name), true).empty()) ++removed;
	};
	const std::int64_t stale_before = now_ns() - k_stale_tmp_seconds * 1'000'000'000;
	for (const beneath::listed_entry& entry : listing.entries) {
		if (entry.kind != beneath::entry_kind::file) continue;
		const std::string name = entry.name.generic_string();
		const bool is_entry = is_entry_name(name);
		if (!is_entry && !is_tmp_name(name)) continue;
		beneath::read_request look;
		look.root = m_root;
		look.folder = folder;
		look.leaf = entry.name;
		look.max_bytes = k_max_entry_bytes;
		look.stamp_only = true;
		std::vector<std::byte> nothing;
		const beneath::read_result seen = beneath::read_beneath(look, nothing);
		if (seen.status == beneath::read_status::too_large || (seen.status == beneath::read_status::ok && !seen.stamp)) {
			if (is_entry) remove(name);
			continue;
		}
		if (seen.status != beneath::read_status::ok) continue;
		if (!is_entry) {
			if (seen.stamp->write_time_ns < stale_before) remove(name);
			continue;
		}
		entries.push_back({ name, seen.stamp->write_time_ns, seen.stamp->size });
	}

	std::ranges::sort(entries, [](const kept& a, const kept& b) { return a.written > b.written; });
	std::uint64_t total = 0;
	for (std::size_t i = 0; i < entries.size(); ++i) {
		total += entries[i].size;
		if (i >= m_limits.max_entries || total > m_limits.max_bytes) {
			remove(entries[i].name);
		}
	}
	return removed;
}
