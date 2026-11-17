#include <snk/resource/res_desc_binary.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <boost/json/string.hpp>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_desc_binary_hash_cache.h>
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
			// Only a vfs resolver gives one: the file did not change while it was read.
			std::optional<::platform::beneath::file_stamp> stamp;
		};

		// `file` from the layer `anchor` comes from. A resolver that is not a vfs one has no layers and no size before a
		// read: its own read, the size checked after.
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
				out.stamp = result.stamp;
				return out;
			}
			const auto data = lease->resolve(file);
			data->wait();
			if (data->has_error()) {
				out.status = lease->exists(file) ? ::platform::beneath::read_status::failed : ::platform::beneath::read_status::missing;
				out.error = data->error_msg;
			} else if (data->data.size() > max_bytes) {
				out.status = ::platform::beneath::read_status::too_large;
			} else {
				out.status = ::platform::beneath::read_status::ok;
				out.bytes = data->data;
			}
			return out;
		}

		// Only a vfs resolver has stamps.
		static std::optional<::platform::beneath::file_stamp> stamp_beside(resource_system& system, const tag& anchor, const tag& file)
		{
			const resource_system::resolver_lease lease = system.root().resolver_for(file.protocol());
			const auto* vfs = lease ? dynamic_cast<const vfs_resolver*>(lease.operator->()) : nullptr;
			return vfs != nullptr ? vfs->stamp_beside(anchor, file) : std::nullopt;
		}

		static bool exists_beside(resource_system& system, const tag& anchor, const tag& file)
		{
			const resource_system::resolver_lease lease = system.root().resolver_for(file.protocol());
			if (!lease) {
				return false;
			}
			if (const auto* vfs = dynamic_cast<const vfs_resolver*>(lease.operator->())) {
				return vfs->exists_beside(anchor, file);
			}
			return lease->exists(file);
		}

		// Whether the protocol of `anchor` is mounted over several folders, where its layer is found again by name.
		static bool layered(resource_system& system, const tag& anchor)
		{
			const resource_system::resolver_lease lease = system.root().resolver_for(anchor.protocol());
			const auto* vfs = lease ? dynamic_cast<const vfs_resolver*>(lease.operator->()) : nullptr;
			return vfs != nullptr && vfs->root_count() > 1;
		}

		static bool writes_beside(resource_system& system, const tag& anchor)
		{
			const resource_system::resolver_lease lease = system.root().resolver_for(anchor.protocol());
			const auto* vfs = lease ? dynamic_cast<const vfs_resolver*>(lease.operator->()) : nullptr;
			return vfs != nullptr && vfs->writes_beside(anchor);
		}

		// Throws std::logic_error for a tag outside limit_writes_to.
		static file_operation commit(resource_system& system, const tag& staged, const tag& target, const std::vector<std::byte>& bytes)
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
			return vfs->commit_staged(staged, target, bytes);
		}

		// Throws std::logic_error for a tag outside limit_writes_to.
		static file_operation remove_if_same(resource_system& system, const tag& staged, const std::vector<std::byte>& bytes)
		{
			system.check_writable(staged);
			const resource_system::resolver_lease lease = system.root().resolver_for(staged.protocol());
			auto* vfs = lease ? dynamic_cast<vfs_resolver*>(lease.operator->()) : nullptr;
			if (vfs == nullptr) {
				return file_operation::failure(std::format("protocol '{}' cannot remove a staged file", staged.protocol()));
			}
			return vfs->remove_if_same(staged, bytes);
		}
	};
}

namespace
{

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

	// Everything a range can be checked for without the file. `extent` is where the last range ends.
	std::optional<res::desc_binary_data> check_ranges(std::string_view file_name, std::span<const res::binary_range> ranges,
		std::uint64_t& extent)
	{
		extent = 0;
		if (ranges.size() > res::desc_binary::max_ranges) {
			return refuse(res::desc_binary_error::range, std::format("{}: {} ranges in one request, at most {} are read",
				file_name, ranges.size(), res::desc_binary::max_ranges));
		}
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

	res::desc_binary_data from_read(std::string_view file_name, res::detail::desc_binary_access::read_outcome&& read,
		std::span<const res::binary_range> ranges, bool verify_hash, std::uint64_t max_bytes)
	{
		using ::platform::beneath::read_status;
		switch (read.status) {
		case read_status::ok:
			return res::desc_binary::parse(file_name, std::move(read.bytes), ranges, verify_hash);
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

	// commit mode, the save lock held: the verified staged bytes become the binary file.
	void commit_recovered(res::resource_system& system, const res::tag& desc, const res::tag& staged, const res::tag& binary,
		const std::vector<std::byte>& bytes)
	{
		if (!res::detail::desc_binary_access::writes_beside(system, desc)) {
			return;
		}
		try {
			if (const auto committed = res::detail::desc_binary_access::commit(system, staged, binary, bytes); !committed) {
				egLOG_WARN("resource/desc_binary", "Could not finish the save of '{}': {}", binary.view(), committed.error);
			}
		} catch (const std::logic_error& ex) {
			egLOG_WARN("resource/desc_binary", "Could not finish the save of '{}': {}", binary.view(), ex.what());
		}
	}

	// commit mode, the save lock held: the binary file matches the desc, so a staged file is left over.
	void remove_stale(res::resource_system& system, const res::tag& desc, const res::tag& staged, std::uint64_t max_bytes)
	{
		using access = res::detail::desc_binary_access;
		if (!access::writes_beside(system, desc)) {
			return;
		}
		const auto stale = access::read_beside(system, desc, staged, max_bytes);
		if (stale.status != ::platform::beneath::read_status::ok) {
			return;
		}
		try {
			if (const auto removed = access::remove_if_same(system, staged, stale.bytes); !removed) {
				egLOG_WARN("resource/desc_binary", "A stale staged file stays: {}", removed.error);
			} else {
				egLOG_INFO("resource/desc_binary", "Removed '{}': the binary file next to it already matches the desc", staged.view());
			}
		} catch (const std::logic_error& ex) {
			egLOG_WARN("resource/desc_binary", "A stale staged file '{}' stays: {}", staged.view(), ex.what());
		}
	}

	// A time this close to now may still change within the same tick of the file system's clock (git's "racy" files).
	bool settled(const res::desc_binary_pair_key& key)
	{
		const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		const std::int64_t limit = now - res::detail::desc_binary_settle_ns().load(std::memory_order_relaxed);
		return key.binary_stamp.write_time_ns <= limit && key.binary_stamp.change_time_ns <= limit
			&& key.desc_stamp.write_time_ns <= limit && key.desc_stamp.change_time_ns <= limit;
	}

	res::desc_binary_data read_pair(res::resource_system& system, const res::tag& desc, const res::tag& binary, const res::tag& staged,
		std::span<const res::binary_range> ranges, bool verify_hash, std::uint64_t max_bytes, bool commit)
	{
		using access = res::detail::desc_binary_access;
		using ::platform::beneath::read_status;
		const std::string binary_name{ binary.view() };
		// Over several folders the desc's layer is found again by name: only the hashes tell its own binary file.
		const bool layered = access::layered(system, desc);
		// A staged file means a save did not finish: then the binary file is checked to tell which of the two is current.
		const bool has_staged = access::exists_beside(system, desc, staged);
		const bool asked = verify_hash && system.verify_desc_binary_hash();
		bool check = asked || layered || has_staged;
		res::desc_binary_data result;
		{
			auto primary = access::read_beside(system, desc, binary, max_bytes);
			std::shared_ptr<res::desc_binary_hash_cache> cache;
			std::optional<res::desc_binary_pair_key> key;
			if (asked && !layered && !has_staged && primary.status == read_status::ok && primary.stamp) {
				cache = system.desc_binary_hash_cache_ptr();
			}
			if (cache) {
				if (const auto desc_stamp = access::stamp_beside(system, desc, desc)) {
					key = res::desc_binary_pair_key{ binary_name, *primary.stamp, *desc_stamp, res::desc_binary_ranges_sha256(ranges) };
				}
			}
			const bool is_settled = key && settled(*key);
			if (is_settled && cache->verified(*key)) {
				check = false;
			}
			result = from_read(binary_name, std::move(primary), ranges, check, max_bytes);
			if (result && check && is_settled) {
				cache->remember(*key);
			}
		}
		if (!has_staged) {
			return result;
		}
		if (result) {
			if (commit) {
				remove_stale(system, desc, staged, max_bytes);
			}
			return result;
		}

		auto pending = access::read_beside(system, desc, staged, max_bytes);
		if (pending.status != read_status::ok) {
			egLOG_WARN("resource/desc_binary", "{}; the staged file next to it cannot be read either", result.message);
			return result;
		}
		res::desc_binary_data recovered = res::desc_binary::parse(staged.view(), std::move(pending.bytes), ranges, true);
		if (!recovered) {
			egLOG_WARN("resource/desc_binary", "{}; the staged file does not match the desc either: {}", result.message, recovered.message);
			return result;
		}
		recovered.from_staged = true;
		egLOG_WARN("resource/desc_binary", "{}; a save did not finish, using '{}', which matches the desc", result.message, staged.view());
		if (commit) {
			commit_recovered(system, desc, staged, binary, recovered.file);
		}
		return recovered;
	}
}

std::atomic<std::uint64_t>& res::detail::desc_binary_hashed_ranges()
{
	static std::atomic<std::uint64_t> hashed{ 0 };
	return hashed;
}

std::atomic<std::int64_t>& res::detail::desc_binary_settle_ns()
{
	static std::atomic<std::int64_t> settle{ 2'000'000'000 };
	return settle;
}

res::desc_binary_data res::desc_binary::parse(std::string_view file_name, std::span<const std::byte> file,
	std::span<const binary_range> ranges, bool verify_hash)
{
	return parse(file_name, std::vector<std::byte>(file.begin(), file.end()), ranges, verify_hash);
}

res::desc_binary_data res::desc_binary::parse(std::string_view file_name, std::vector<std::byte>&& file,
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
	if (std::memcmp(file.data(), magic.data(), magic.size()) != 0) {
		return refuse(desc_binary_error::header, std::format("{}: not a desc binary file (no SNKB magic)", file_name));
	}
	std::uint32_t found_version = 0;
	for (std::size_t i = 0; i < 4; ++i) {
		found_version |= static_cast<std::uint32_t>(file[magic.size() + i]) << (8 * i);
	}
	if (found_version != version) {
		return refuse(desc_binary_error::header,
			std::format("{}: format version {}, this engine reads version {}", file_name, found_version, version));
	}

	desc_binary_data out;
	out.file = std::move(file);
	const std::span<const std::byte> whole(out.file);
	out.ranges.reserve(ranges.size());
	for (std::size_t i = 0; i < ranges.size(); ++i) {
		const binary_range& range = ranges[i];
		if (range.offset + range.size > whole.size()) {
			return refuse(desc_binary_error::range,
				std::format("{}: {} lies outside the file ({} bytes)", file_name, describe(i, range), whole.size()));
		}
		const auto bytes = whole.subspan(static_cast<std::size_t>(range.offset), static_cast<std::size_t>(range.size));
		if (verify_hash) {
			detail::desc_binary_hashed_ranges().fetch_add(1, std::memory_order_relaxed);
			if (cmn::sha256_hex(bytes) != range.sha256) {
				return refuse(desc_binary_error::hash,
					std::format("{}: {} does not match its SHA-256 in the desc", file_name, describe(i, range)));
			}
		}
		out.ranges.push_back(bytes);
	}
	return out;
}

res::desc_binary_data res::desc_binary::read(resource_system& system, const tag& desc, std::span<const binary_range> ranges,
	bool verify_hash, std::uint64_t max_bytes, desc_binary_recovery recovery)
{
	const auto binary = binary_tag(desc);
	const auto staged = staged_tag(desc);
	if (!binary || !staged) {
		return refuse(desc_binary_error::missing, std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view()));
	}
	std::uint64_t extent = 0;
	if (auto refused = check_ranges(binary->view(), ranges, extent)) {
		return std::move(*refused);
	}
	if (extent > max_bytes) {
		return refuse(desc_binary_error::too_large,
			std::format("{}: the ranges reach byte {}, over the limit of {} bytes", binary->view(), extent, max_bytes));
	}
	if (recovery == desc_binary_recovery::commit) {
		const save_lock lock(desc);
		return read_pair(system, desc, *binary, *staged, ranges, verify_hash, max_bytes, true);
	}
	return read_pair(system, desc, *binary, *staged, ranges, verify_hash, max_bytes, false);
}

namespace
{
	void require_guard(const res::desc_binary::save_lock& lock, const res::tag& desc, std::string_view step)
	{
		if (!lock.guards(desc)) {
			throw std::logic_error(std::format("desc_binary::{} of '{}' under the save lock of another desc", step, desc.view()));
		}
	}
}

res::desc_binary_written res::desc_binary::write_pending(resource_system& system, const save_lock& lock, const tag& desc,
	std::span<const binary_chunk> chunks)
{
	require_guard(lock, desc, "write_pending");
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
		return out;
	}
	out.file = std::move(*file);
	return out;
}

res::file_operation res::desc_binary::commit_pending(resource_system& system, const save_lock& lock, const tag& desc,
	const desc_binary_written& written)
{
	require_guard(lock, desc, "commit_pending");
	const auto binary = binary_tag(desc);
	const auto staged = staged_tag(desc);
	if (!binary || !staged) {
		return file_operation::failure(std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view()));
	}
	if (!written || written.file.empty()) {
		return file_operation::failure(std::format("'{}': nothing was staged to commit", binary->view()));
	}
	return detail::desc_binary_access::commit(system, *staged, *binary, written.file);
}

res::desc_binary::previous_desc res::desc_binary::read_previous_desc(resource_system& system, const save_lock& lock,
	const tag& desc)
{
	using access = detail::desc_binary_access;
	require_guard(lock, desc, "read_previous_desc");
	previous_desc out;
	if (!system.exists(desc)) {
		return out;
	}
	access::read_outcome read = access::read_beside(system, desc, desc, max_geometry_json_bytes);
	switch (read.status) {
	case ::platform::beneath::read_status::ok:
		out.bytes = std::move(read.bytes);
		out.from = access::layered(system, desc) && !access::writes_beside(system, desc) ? previous_desc::origin::lower_layer
			: previous_desc::origin::top_layer;
		break;
	case ::platform::beneath::read_status::missing:
		break;
	default:
		out.from = previous_desc::origin::unreadable;
		out.error = read.error.empty() ? std::string{ "it cannot be read" } : std::move(read.error);
		break;
	}
	return out;
}

res::file_operation res::desc_binary::take_back(resource_system& system, const save_lock& lock, const tag& desc,
	const desc_binary_written& written, const std::vector<std::byte>& written_desc, const previous_desc& previous)
{
	require_guard(lock, desc, "take_back");
	const auto staged = staged_tag(desc);
	if (!staged) {
		return file_operation::failure(std::format("'{}' is not named <name>.<ext>.desc: it has no binary file", desc.view()));
	}
	switch (previous.from) {
	case previous_desc::origin::top_layer:
		if (!system.store_sync(desc, previous.bytes)) {
			return file_operation::failure(std::format("the previous '{}' could not be written back (see the log)", desc.view()));
		}
		break;
	case previous_desc::origin::none:
	case previous_desc::origin::lower_layer:
		// A copy of a lower layer's desc written on top would have no binary file beside it.
		if (const file_operation removed = detail::desc_binary_access::remove_if_same(system, desc, written_desc); !removed) {
			return file_operation::failure(std::format("'{}' could not be removed: {}", desc.view(), removed.error));
		}
		break;
	case previous_desc::origin::unreadable:
		egLOG_WARN("resource/desc_binary", "'{}' is not taken back: the desc before the save could not be read ({})",
			desc.view(), previous.error);
		return file_operation::failure(std::format("the desc before the save could not be read ({}), so '{}' was not taken back",
			previous.error, desc.view()));
	}
	// No desc needs the staged file any more: a leftover is only stale, a reader ignores it.
	if (!written.file.empty()) {
		if (const file_operation removed = detail::desc_binary_access::remove_if_same(system, *staged, written.file); !removed) {
			egLOG_WARN("resource/desc_binary", "A stale staged file '{}' stays: {}", staged->view(), removed.error);
		}
	}
	return file_operation::success();
}

res::desc_binary_written res::desc_binary::save(resource_system& system, const tag& desc, std::span<const binary_chunk> chunks,
	const desc_writer& write_desc)
{
	const save_lock lock(desc);
	desc_binary_written out = write_pending(system, lock, desc, chunks);
	if (!out) {
		out.failed_step = desc_binary_save_step::staging;
		return out;
	}
	if (!write_desc || !write_desc(out.ranges)) {
		out.error = std::format("'{}' was not written: the save stopped before the commit", desc.view());
		out.failed_step = desc_binary_save_step::desc;
		return out;
	}
	// The desc is on disk: one more try before the caller has to take it back.
	file_operation committed = commit_pending(system, lock, desc, out);
	if (!committed) {
		egLOG_WARN("resource/desc_binary", "'{}': the commit of its binary file failed, tried again: {}", desc.view(), committed.error);
		committed = commit_pending(system, lock, desc, out);
	}
	if (!committed) {
		out.error = std::format("'{}' was written, its binary file was not committed: {}", desc.view(), committed.error);
		out.failed_step = desc_binary_save_step::commit;
	}
	return out;
}
