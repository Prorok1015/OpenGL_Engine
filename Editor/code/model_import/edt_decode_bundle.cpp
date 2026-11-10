#include "edt_decode_bundle.h"
#include <boost/json/parse_options.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/stream_parser.hpp>
#include <snk/platform/plt_beneath.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <string_view>
#include <unordered_set>

namespace {

constexpr std::array<char, 4> k_magic = { 'S', 'N', 'K', 'B' };
constexpr std::uint32_t k_version = 1;
constexpr std::string_view k_tag_prefix = "memory://";

void put_u32(std::string& out, std::uint32_t value)
{
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void put_u64(std::string& out, std::uint64_t value)
{
	for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

// A reader over the bytes of the file that knows how much of them is left, so a declared size is checked before it is used.
class bounded_reader
{
public:
	explicit bounded_reader(const std::vector<std::byte>& bytes) : m_bytes(bytes), m_left(bytes.size()) {}

	std::uint64_t left() const { return m_left; }

	bool read(void* into, std::uint64_t count)
	{
		if (count > m_left) return false;
		if (count > 0) {
			std::memcpy(into, m_bytes.data() + m_position, static_cast<std::size_t>(count));
			m_position += static_cast<std::size_t>(count);
		}
		m_left -= count;
		return true;
	}

	bool u32(std::uint32_t& value)
	{
		unsigned char raw[4];
		if (!read(raw, sizeof(raw))) return false;
		value = 0;
		for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(raw[i]) << (8 * i);
		return true;
	}

	bool u64(std::uint64_t& value)
	{
		unsigned char raw[8];
		if (!read(raw, sizeof(raw))) return false;
		value = 0;
		for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(raw[i]) << (8 * i);
		return true;
	}

private:
	const std::vector<std::byte>& m_bytes;
	std::size_t m_position = 0;
	std::uint64_t m_left;
};

// The most a bundle under `limits` can take: the head, the manifest, and per blob its tag, its two sizes and its bytes.
std::uint64_t largest_bundle(const edt::bundle_limits& limits)
{
	constexpr std::uint64_t k_head = 4 + 4 + 8 + 8;
	return k_head + limits.max_manifest_bytes + limits.max_total_bytes + limits.max_blobs * (4 + 8 + limits.max_tag_bytes);
}

}

bool edt::is_bundle_tag(const std::string& tag, std::size_t max_bytes)
{
	if (tag.size() <= k_tag_prefix.size() || tag.size() > max_bytes || !tag.starts_with(k_tag_prefix)) return false;
	const std::string_view path = std::string_view(tag).substr(k_tag_prefix.size());
	if (path.front() == '/' || path.back() == '/') return false;
	std::string_view segment_rest = path;
	while (!segment_rest.empty()) {
		const std::size_t slash = segment_rest.find('/');
		const std::string_view segment = segment_rest.substr(0, slash);
		if (segment.empty() || segment == "." || segment == "..") return false;
		if (slash == std::string_view::npos) break;
		segment_rest.remove_prefix(slash + 1);
	}
	return std::none_of(path.begin(), path.end(), [](char c) {
		return static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == '\\' || c == ':';
	});
}

bool edt::write_bundle(const std::filesystem::path& file, const decode_bundle& bundle, std::string& error)
{
	const std::string manifest = boost::json::serialize(bundle.manifest);
	std::string head(k_magic.data(), k_magic.size());
	put_u32(head, k_version);
	put_u64(head, manifest.size());
	put_u64(head, bundle.blobs.size());

	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) {
		error = "the bundle file cannot be written";
		return false;
	}
	out.write(head.data(), static_cast<std::streamsize>(head.size()));
	out.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
	for (const bundle_blob& blob : bundle.blobs) {
		if (!is_bundle_tag(blob.tag, bundle_limits{}.max_tag_bytes)) {
			error = std::format("'{}' is not a tag a bundle may carry", blob.tag.substr(0, 200));
			return false;
		}
		std::string entry;
		put_u32(entry, static_cast<std::uint32_t>(blob.tag.size()));
		entry += blob.tag;
		put_u64(entry, blob.bytes.size());
		out.write(entry.data(), static_cast<std::streamsize>(entry.size()));
		if (!blob.bytes.empty()) out.write(reinterpret_cast<const char*>(blob.bytes.data()), static_cast<std::streamsize>(blob.bytes.size()));
	}
	out.flush();
	if (!out) {
		error = "the bundle file could not be written to the end";
		return false;
	}
	return true;
}

edt::bundle_read_result edt::read_bundle(const std::filesystem::path& file, const bundle_limits& limits)
{
	bundle_read_result result;
	const auto refuse = [&](std::string why) {
		result.bundle.reset();
		result.error = std::move(why);
		return result;
	};

	// The file was made by the process that is being distrusted: it is opened without following a link and without
	// blocking (a FIFO cannot hold the worker), and checked to be a regular file through the handle it is read from, so
	// nothing swapped between a check and the read is seen. Too big to be a bundle under the limits: nothing is read.
	std::error_code ec;
	const std::filesystem::path folder = std::filesystem::canonical(file.parent_path(), ec);
	if (ec || file.filename().empty()) return refuse("the bundle file cannot be opened");
	platform::beneath::read_request request;
	request.root = folder;
	request.leaf = file.filename();
	request.max_bytes = largest_bundle(limits);
	std::vector<std::byte> contents;
	const platform::beneath::read_result read = platform::beneath::read_beneath(request, contents);
	switch (read.status) {
	case platform::beneath::read_status::ok: break;
	case platform::beneath::read_status::not_regular: return refuse("the bundle is not a regular file");
	case platform::beneath::read_status::too_large: return refuse("the bundle is bigger than the limits allow");
	case platform::beneath::read_status::missing: return refuse("the bundle file is missing");
	default: return refuse("the bundle file cannot be read");
	}
	bounded_reader reader(contents);

	std::array<char, 4> magic{};
	std::uint32_t version = 0;
	std::uint64_t manifest_size = 0;
	std::uint64_t blob_count = 0;
	if (!reader.read(magic.data(), magic.size()) || magic != k_magic) return refuse("not a decoder bundle");
	if (!reader.u32(version) || version != k_version) return refuse("a decoder bundle of another version");
	if (!reader.u64(manifest_size) || !reader.u64(blob_count)) return refuse("the bundle is truncated");
	if (manifest_size > limits.max_manifest_bytes) return refuse("the manifest of the bundle is too big");
	if (blob_count > limits.max_blobs) return refuse("the bundle holds too many resources");
	if (manifest_size > reader.left()) return refuse("the bundle is truncated");

	decode_bundle bundle;
	{
		std::string manifest_text(static_cast<std::size_t>(manifest_size), '\0');
		if (!reader.read(manifest_text.data(), manifest_size)) return refuse("the bundle is truncated");
		boost::json::parse_options options;
		options.max_depth = limits.max_json_depth;
		boost::json::stream_parser parser(boost::json::storage_ptr{}, options);
		boost::system::error_code json_error;
		parser.write(manifest_text.data(), manifest_text.size(), json_error);
		if (!json_error) parser.finish(json_error);
		if (json_error) return refuse("the manifest of the bundle is not valid JSON within the limits");
		boost::json::value value = parser.release();
		if (!value.is_object()) return refuse("the manifest of the bundle is not an object");
		bundle.manifest = std::move(value.as_object());
	}

	std::uint64_t total = 0;
	std::unordered_set<std::string> seen;
	bundle.blobs.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(blob_count, 64)));
	for (std::uint64_t i = 0; i < blob_count; ++i) {
		std::uint32_t tag_size = 0;
		if (!reader.u32(tag_size)) return refuse("the bundle is truncated");
		if (tag_size > limits.max_tag_bytes || tag_size > reader.left()) return refuse("a resource of the bundle has a tag that is too long");
		std::string tag(tag_size, '\0');
		if (!reader.read(tag.data(), tag_size)) return refuse("the bundle is truncated");
		if (!is_bundle_tag(tag, limits.max_tag_bytes)) return refuse("a resource of the bundle has a tag that is not allowed");
		if (!seen.insert(tag).second) return refuse("a resource of the bundle is there twice");

		std::uint64_t blob_size = 0;
		if (!reader.u64(blob_size)) return refuse("the bundle is truncated");
		if (blob_size > limits.max_blob_bytes) return refuse("a resource of the bundle is too big");
		if (blob_size > reader.left()) return refuse("a resource of the bundle is bigger than the file holds");
		if (total + blob_size > limits.max_total_bytes) return refuse("the resources of the bundle together are too big");
		total += blob_size;

		bundle_blob blob;
		blob.tag = std::move(tag);
		blob.bytes.resize(static_cast<std::size_t>(blob_size));
		if (!reader.read(blob.bytes.data(), blob_size)) return refuse("the bundle is truncated");
		bundle.blobs.push_back(std::move(blob));
	}
	if (reader.left() != 0) return refuse("the bundle has bytes after its end");

	result.bundle = std::move(bundle);
	return result;
}
