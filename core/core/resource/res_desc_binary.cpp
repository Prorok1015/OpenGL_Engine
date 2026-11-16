#include <snk/resource/res_desc_binary.h>
#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>
#include <boost/json/string.hpp>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>

namespace res::detail
{
	struct desc_binary_access
	{
		struct read_outcome
		{
			::platform::beneath::read_status status = ::platform::beneath::read_status::failed;
			std::string error;
			std::vector<std::byte> bytes;
		};

		// `file` from the layer `anchor` comes from. A resolver that is not a vfs one has no layers: its own read.
		static read_outcome read_beside(resource_system& system, const tag& anchor, const tag& file, std::uint64_t max_bytes)
		{
			read_outcome out;
			const resource_system::resolver_lease lease = system.root().resolver_for(file.protocol());
			if (!lease) {
				out.error = system.not_mounted_error(file);
				return out;
			}
			if (const auto* vfs = dynamic_cast<const vfs_resolver*>(lease.operator->())) {
				const auto result = vfs->read_beside(anchor, file, max_bytes, out.bytes);
				out.status = result.status;
				out.error = result.error;
				return out;
			}
			const auto data = lease->resolve(file);
			data->wait();
			if (data->has_error()) {
				out.status = ::platform::beneath::read_status::missing;
				out.error = data->error_msg;
			} else if (data->data.size() > max_bytes) {
				out.status = ::platform::beneath::read_status::too_large;
			} else {
				out.status = ::platform::beneath::read_status::ok;
				out.bytes = data->data;
			}
			return out;
		}

		static bool writes_beside(resource_system& system, const tag& anchor)
		{
			const resource_system::resolver_lease lease = system.root().resolver_for(anchor.protocol());
			const auto* vfs = lease ? dynamic_cast<const vfs_resolver*>(lease.operator->()) : nullptr;
			return vfs != nullptr && vfs->writes_beside(anchor);
		}

		static file_operation commit(resource_system& system, const tag& staged, const tag& target)
		{
			system.check_writable(staged);
			system.check_writable(target);
			const resource_system::resolver_lease lease = system.root().resolver_for(staged.protocol());
			if (!lease) {
				return file_operation::failure(system.not_mounted_error(staged));
			}
			auto* vfs = dynamic_cast<vfs_resolver*>(lease.operator->());
			if (vfs == nullptr) {
				return file_operation::failure(std::format("protocol '{}' cannot commit a staged file", staged.protocol()));
			}
			return vfs->commit_staged(staged, target);
		}
	};
}

namespace
{
	constexpr std::array<char, 4> k_magic{ 'S', 'N', 'K', 'B' };
	constexpr std::string_view k_desc_suffix = ".desc";

	res::desc_binary_data refuse(res::desc_binary_error kind, std::string message)
	{
		res::desc_binary_data out;
		out.error = kind;
		out.message = std::move(message);
		return out;
	}

	std::string describe(std::size_t index, const res::binary_range& range)
	{
		return std::format("range {} (offset {}, size {})", index, range.offset, range.size);
	}

	std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	// Everything a range can be checked for without the file. `extent` is where the last range ends.
	std::optional<res::desc_binary_data> check_ranges(std::string_view file_name, std::span<const res::binary_range> ranges,
		std::uint64_t& extent)
	{
		extent = 0;
		std::vector<std::size_t> order;
		order.reserve(ranges.size());
		for (std::size_t i = 0; i < ranges.size(); ++i) {
			const res::binary_range& range = ranges[i];
			const auto layout = res::parse_binary_layout(range.layout);
			if (!layout) {
				return refuse(res::desc_binary_error::layout,
					std::format("{}: {} has an unknown layout '{}'", file_name, describe(i, range), range.layout));
			}
			if (range.size % layout->element_bytes() != 0) {
				return refuse(res::desc_binary_error::layout, std::format("{}: {} is not a whole number of '{}' elements ({} bytes)",
					file_name, describe(i, range), range.layout, layout->element_bytes()));
			}
			if (range.offset < res::desc_binary::header_bytes) {
				return refuse(res::desc_binary_error::range,
					std::format("{}: {} starts inside the {}-byte header", file_name, describe(i, range), res::desc_binary::header_bytes));
			}
			if (range.size > std::numeric_limits<std::uint64_t>::max() - range.offset) {
				return refuse(res::desc_binary_error::range, std::format("{}: {} ends past 2^64", file_name, describe(i, range)));
			}
			extent = std::max(extent, range.offset + range.size);
			if (range.size != 0) {
				order.push_back(i);
			}
		}
		std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ranges[a].offset < ranges[b].offset; });
		for (std::size_t k = 1; k < order.size(); ++k) {
			const res::binary_range& before = ranges[order[k - 1]];
			const res::binary_range& after = ranges[order[k]];
			if (before.offset + before.size > after.offset) {
				return refuse(res::desc_binary_error::range, std::format("{}: {} overlaps {}", file_name,
					describe(order[k - 1], before), describe(order[k], after)));
			}
		}
		return std::nullopt;
	}

	res::desc_binary_data from_read(std::string_view file_name, const res::detail::desc_binary_access::read_outcome& read,
		std::span<const res::binary_range> ranges, bool verify_hash, std::uint64_t max_bytes)
	{
		using ::platform::beneath::read_status;
		switch (read.status) {
		case read_status::ok:
			return res::desc_binary::parse(file_name, read.bytes, ranges, verify_hash);
		case read_status::missing:
			return refuse(res::desc_binary_error::missing, std::format("{}: the file does not exist", file_name));
		case read_status::too_large:
			return refuse(res::desc_binary_error::too_large,
				std::format("{}: the file is larger than {} bytes and was not read", file_name, max_bytes));
		case read_status::not_regular:
			return refuse(res::desc_binary_error::io, std::format("{}: not a regular file", file_name));
		case read_status::failed:
			break;
		}
		return refuse(res::desc_binary_error::io, std::format("{}: {}", file_name, read.error));
	}

	bool is_lower_hex(std::string_view text)
	{
		return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
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
		if (field->is_uint64()) {
			return field->get_uint64();
		}
		if (field->is_int64() && field->get_int64() >= 0) {
			return static_cast<std::uint64_t>(field->get_int64());
		}
		error = std::format("'{}' of a binary range is not a non-negative integer", key);
		return std::nullopt;
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
	std::memcpy(file.data(), k_magic.data(), k_magic.size());
	for (std::size_t i = 0; i < 4; ++i) {
		file[k_magic.size() + i] = static_cast<std::byte>((version >> (8 * i)) & 0xffu);
	}
	for (std::size_t i = 0; i < chunks.size(); ++i) {
		if (!chunks[i].bytes.empty()) {
			std::memcpy(file.data() + ranges[i].offset, chunks[i].bytes.data(), chunks[i].bytes.size());
		}
	}
	return file;
}

res::desc_binary_data res::desc_binary::parse(std::string_view file_name, std::span<const std::byte> file,
	std::span<const binary_range> ranges, bool verify_hash)
{
	std::uint64_t extent = 0;
	if (auto refused = check_ranges(file_name, ranges, extent)) {
		return std::move(*refused);
	}
	if (file.size() < header_bytes) {
		return refuse(desc_binary_error::header,
			std::format("{}: {} bytes, shorter than the {}-byte header", file_name, file.size(), header_bytes));
	}
	if (std::memcmp(file.data(), k_magic.data(), k_magic.size()) != 0) {
		return refuse(desc_binary_error::header, std::format("{}: not a desc binary file (no SNKB magic)", file_name));
	}
	std::uint32_t found_version = 0;
	for (std::size_t i = 0; i < 4; ++i) {
		found_version |= static_cast<std::uint32_t>(file[k_magic.size() + i]) << (8 * i);
	}
	if (found_version != version) {
		return refuse(desc_binary_error::header,
			std::format("{}: format version {}, this engine reads version {}", file_name, found_version, version));
	}

	desc_binary_data out;
	out.ranges.reserve(ranges.size());
	for (std::size_t i = 0; i < ranges.size(); ++i) {
		const binary_range& range = ranges[i];
		if (range.offset + range.size > file.size()) {
			return refuse(desc_binary_error::range,
				std::format("{}: {} lies outside the file ({} bytes)", file_name, describe(i, range), file.size()));
		}
		const auto bytes = file.subspan(static_cast<std::size_t>(range.offset), static_cast<std::size_t>(range.size));
		if (verify_hash && cmn::sha256_hex(bytes) != range.sha256) {
			return refuse(desc_binary_error::hash,
				std::format("{}: {} does not match its SHA-256 in the desc", file_name, describe(i, range)));
		}
		out.ranges.emplace_back(bytes.begin(), bytes.end());
	}
	return out;
}

res::desc_binary_data res::desc_binary::read(resource_system& system, const tag& desc, std::span<const binary_range> ranges,
	bool verify_hash, std::uint64_t max_bytes)
{
	const auto binary = binary_tag(desc);
	const auto staged = staged_tag(desc);
	if (!binary || !staged) {
		return refuse(desc_binary_error::missing, std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view()));
	}
	const std::string binary_name{ binary->view() };
	std::uint64_t extent = 0;
	if (auto refused = check_ranges(binary_name, ranges, extent)) {
		return std::move(*refused);
	}
	if (extent > max_bytes) {
		return refuse(desc_binary_error::too_large,
			std::format("{}: the ranges reach byte {}, over the limit of {} bytes", binary_name, extent, max_bytes));
	}

	using access = detail::desc_binary_access;
	const auto primary = access::read_beside(system, desc, *binary, max_bytes);
	const auto pending = access::read_beside(system, desc, *staged, max_bytes);
	const bool has_staged = pending.status == ::platform::beneath::read_status::ok;
	desc_binary_data result = from_read(binary_name, primary, ranges, verify_hash || has_staged, max_bytes);
	if (result || !has_staged) {
		return result;
	}

	desc_binary_data recovered = parse(staged->view(), pending.bytes, ranges, true);
	if (!recovered) {
		egLOG_WARN("resource/desc_binary", "{}; the staged file does not match the desc either: {}", result.message, recovered.message);
		return result;
	}
	recovered.from_staged = true;
	egLOG_WARN("resource/desc_binary", "{}; a save did not finish, using '{}', which matches the desc", result.message, staged->view());
	if (access::writes_beside(system, desc)) {
		try {
			if (const auto committed = commit_pending(system, desc); !committed) {
				egLOG_WARN("resource/desc_binary", "Could not finish the save of '{}': {}", binary_name, committed.error);
			}
		} catch (const std::logic_error& ex) {
			egLOG_WARN("resource/desc_binary", "Could not finish the save of '{}': {}", binary_name, ex.what());
		}
	}
	return recovered;
}

res::desc_binary_written res::desc_binary::write_pending(resource_system& system, const tag& desc, std::span<const binary_chunk> chunks)
{
	desc_binary_written out;
	const auto staged = staged_tag(desc);
	if (!staged) {
		out.error = std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view());
		return out;
	}
	auto file = encode(chunks, out.ranges, out.error);
	if (!file) {
		out.ranges.clear();
		return out;
	}
	if (!system.store_sync(*staged, *file)) {
		out.ranges.clear();
		out.error = std::format("'{}' could not be written (see the log)", staged->view());
	}
	return out;
}

res::file_operation res::desc_binary::commit_pending(resource_system& system, const tag& desc)
{
	const auto binary = binary_tag(desc);
	const auto staged = staged_tag(desc);
	if (!binary || !staged) {
		return file_operation::failure(std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view()));
	}
	return detail::desc_binary_access::commit(system, *staged, *binary);
}
