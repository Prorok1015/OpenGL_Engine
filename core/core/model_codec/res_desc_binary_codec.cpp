#include <snk/resource/res_desc_binary.h>
#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <boost/json/string.hpp>
#include <snk/common/hash/sha256.h>

// The part of res_desc_binary.h that needs no resource_system (#1077): the import encodes and locks with it in the model
// decoder, which has none. read, write_pending, commit_pending, take_back and save are in resource_system.
namespace
{
	constexpr std::string_view k_desc_suffix = ".desc";

	std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	bool is_lower_hex(std::string_view text)
	{
		return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}

	// One recursive mutex per binary file, alive while a save_lock holds it. One table per process: the shared engine
	// carries this archive, and a binary that links the engine never links it again (snk_check_engine_copies).
	struct save_lock_table
	{
		std::mutex mutex;
		std::unordered_map<std::string, std::weak_ptr<std::recursive_mutex>> entries;
	};

	std::shared_ptr<std::recursive_mutex> save_mutex_of(const res::tag& desc)
	{
		static save_lock_table table;
		const auto binary = res::desc_binary::binary_tag(desc);
		const std::string key{ binary ? binary->view() : desc.view() };
		std::lock_guard lock(table.mutex);
		std::erase_if(table.entries, [](const auto& entry) { return entry.second.expired(); });
		std::weak_ptr<std::recursive_mutex>& slot = table.entries[key];
		if (auto alive = slot.lock()) {
			return alive;
		}
		auto made = std::make_shared<std::recursive_mutex>();
		slot = made;
		return made;
	}
}

std::optional<res::binary_layout> res::parse_binary_layout(std::string_view text)
{
	binary_layout out;
	std::string_view rest;
	if (text == "u8") {
		return out;
	}
	if (text.starts_with("u32")) {
		out.scalar = binary_scalar::u32;
	} else if (text.starts_with("f32")) {
		out.scalar = binary_scalar::f32;
	} else {
		return std::nullopt;
	}
	rest = text.substr(3);
	if (rest.empty()) {
		return out;
	}
	// "xN" with N written without a leading zero: one spelling per layout.
	if (rest.size() < 2 || rest.size() > 3 || rest[0] != 'x' || rest[1] == '0') {
		return std::nullopt;
	}
	unsigned count = 0;
	for (const char c : rest.substr(1)) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		count = count * 10 + static_cast<unsigned>(c - '0');
	}
	if (count < 1 || count > 32) {
		return std::nullopt;
	}
	out.count = static_cast<std::uint8_t>(count);
	return out;
}

boost::json::object res::to_json(const binary_range& range)
{
	boost::json::object out;
	out["offset"] = range.offset;
	out["size"] = range.size;
	out["sha256"] = range.sha256;
	out["layout"] = range.layout;
	return out;
}

std::optional<res::binary_range> res::binary_range_from_json(const boost::json::value& value, std::string& error)
{
	const auto* object = value.if_object();
	if (object == nullptr) {
		error = "a binary range is not an object";
		return std::nullopt;
	}
	const auto unsigned_of = [&](std::string_view key) -> std::optional<std::uint64_t> {
		const auto* field = object->if_contains(key);
		if (field == nullptr) {
			error = std::format("a binary range has no '{}'", key);
			return std::nullopt;
		}
		// Up to 2^53: a reader that keeps JSON numbers as doubles still gets the exact value.
		constexpr std::uint64_t max_exact = std::uint64_t{ 1 } << 53;
		std::optional<std::uint64_t> found;
		if (field->is_uint64()) {
			found = field->get_uint64();
		} else if (field->is_int64() && field->get_int64() >= 0) {
			found = static_cast<std::uint64_t>(field->get_int64());
		}
		if (!found || *found > max_exact) {
			error = std::format("'{}' of a binary range is not an integer from 0 to 2^53", key);
			return std::nullopt;
		}
		return found;
	};
	const auto text_of = [&](std::string_view key) -> std::optional<std::string> {
		const auto* field = object->if_contains(key);
		const auto* text = field != nullptr ? field->if_string() : nullptr;
		if (text == nullptr) {
			error = std::format("'{}' of a binary range is missing or not a string", key);
			return std::nullopt;
		}
		return std::string(text->data(), text->size());
	};

	const auto offset = unsigned_of("offset");
	if (!offset) {
		return std::nullopt;
	}
	const auto size = unsigned_of("size");
	if (!size) {
		return std::nullopt;
	}
	auto sha256 = text_of("sha256");
	if (!sha256) {
		return std::nullopt;
	}
	auto layout = text_of("layout");
	if (!layout) {
		return std::nullopt;
	}
	if (sha256->size() != 64 || !is_lower_hex(*sha256)) {
		error = "'sha256' of a binary range is not 64 lowercase hex digits";
		return std::nullopt;
	}
	if (!parse_binary_layout(*layout)) {
		error = std::format("'layout' of a binary range is unknown: '{}'", *layout);
		return std::nullopt;
	}
	return binary_range{ *offset, *size, std::move(*sha256), std::move(*layout) };
}

std::optional<res::tag> res::desc_binary::binary_tag(const tag& desc)
{
	const std::string_view name = desc.name();
	if (!name.ends_with(k_desc_suffix)) {
		return std::nullopt;
	}
	// `<name>.<ext>`: a desc kind is always named, so "a.desc" or ".geom.desc" has no binary file.
	const std::string_view stem = name.substr(0, name.size() - k_desc_suffix.size());
	const auto dot = stem.find_last_of('.');
	if (dot == std::string_view::npos || dot == 0 || dot + 1 == stem.size()) {
		return std::nullopt;
	}
	const std::string_view full = desc.view();
	return tag{ std::string(full.substr(0, full.size() - k_desc_suffix.size())) + ".bin" };
}

std::optional<res::tag> res::desc_binary::staged_tag(const tag& desc)
{
	const auto binary = binary_tag(desc);
	if (!binary) {
		return std::nullopt;
	}
	return tag{ std::string(binary->view()) + ".new" };
}

std::optional<std::vector<std::byte>> res::desc_binary::encode(std::span<const binary_chunk> chunks,
	std::vector<binary_range>& ranges, std::string& error)
{
	ranges.clear();
	// A file is read at most max_ranges ranges at a time: one with more could never be read back whole.
	if (chunks.size() > max_ranges) {
		error = std::format("{} chunks in one file, at most {} are read", chunks.size(), max_ranges);
		return std::nullopt;
	}
	ranges.reserve(chunks.size());
	std::uint64_t end = header_bytes;
	for (std::size_t i = 0; i < chunks.size(); ++i) {
		const binary_chunk& chunk = chunks[i];
		const auto layout = parse_binary_layout(chunk.layout);
		if (!layout) {
			error = std::format("chunk {} has an unknown layout '{}'", i, chunk.layout);
			return std::nullopt;
		}
		if (chunk.bytes.size() % layout->element_bytes() != 0) {
			error = std::format("chunk {} ({} bytes) is not a whole number of '{}' elements", i, chunk.bytes.size(), chunk.layout);
			return std::nullopt;
		}
		const std::uint64_t offset = align_up(end, range_alignment);
		if (chunk.bytes.size() > max_file_bytes || offset + chunk.bytes.size() > max_file_bytes) {
			error = std::format("the binary file would be larger than {} bytes", max_file_bytes);
			return std::nullopt;
		}
		ranges.push_back(binary_range{ offset, chunk.bytes.size(), cmn::sha256_hex(chunk.bytes), std::string(chunk.layout) });
		end = offset + chunk.bytes.size();
	}

	std::vector<std::byte> file(static_cast<std::size_t>(end));
	std::memcpy(file.data(), magic.data(), magic.size());
	for (std::size_t i = 0; i < 4; ++i) {
		file[magic.size() + i] = static_cast<std::byte>((version >> (8 * i)) & 0xffu);
	}
	for (std::size_t i = 0; i < chunks.size(); ++i) {
		if (!chunks[i].bytes.empty()) {
			std::memcpy(file.data() + ranges[i].offset, chunks[i].bytes.data(), chunks[i].bytes.size());
		}
	}
	return file;
}

res::desc_binary::save_lock::save_lock(const tag& desc)
	: m_mutex(save_mutex_of(desc))
{
	m_mutex->lock();
}

res::desc_binary::save_lock::~save_lock()
{
	m_mutex->unlock();
}

bool res::desc_binary::save_lock::guards(const tag& desc) const
{
	return m_mutex == save_mutex_of(desc);
}
