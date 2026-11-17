#include <snk/render/geom/rnd_geometry_files.h>
#include "rnd_geometry_binary.h"
#include <snk/common/eng_profiler.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_system.h>
#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

std::string rnd::geometry_write_problem(const res::tag& file, const geometry_desc& geometry, const geometry_file_options& options)
{
	return geometry_write_problem(file, geometry_view_of(geometry), options);
}

rnd::geometry_files rnd::encode_geometry_files(const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options)
{
	return encode_geometry_files(file, geometry_view_of(geometry), options);
}

std::string rnd::write_geometry_files(res::resource_system& system, const res::tag& file, const geometry_desc& geometry,
	const geometry_file_options& options, std::vector<res::tag>* left)
{
	PROFILE_SCOPE("geometry files write");
	const geometry_view view = geometry_view_of(geometry);
	if (std::string problem = geometry_write_problem(file, view, options); !problem.empty()) {
		return problem;
	}
	detail::geometry_chunks chunks;
	detail::fill_geometry_chunks(view, chunks);
	std::string desc_error;
	res::desc_binary::previous_desc previous;
	std::vector<std::byte> written_desc;
	try {
		// Held across save() (it nests) and the take-back of a failed commit: no other save of the pair comes between.
		const res::desc_binary::save_lock lock(file);
		const res::desc_binary_written written = res::desc_binary::save(system, file, chunks.chunks,
			[&](std::span<const res::binary_range> ranges) {
				previous = res::desc_binary::read_previous_desc(system, lock, file);
				written_desc = detail::geometry_desc_bytes(view, options, ranges);
				if (!system.store_sync(file, written_desc)) {
					desc_error = std::format("'{}' could not be written (see the log); its old pair stays readable", file.view());
					return false;
				}
				return true;
			});
		const auto staged = *res::desc_binary::staged_tag(file);
		switch (written.failed_step) {
		case res::desc_binary_save_step::none:
			break;
		case res::desc_binary_save_step::staging:
			return written.error;
		case res::desc_binary_save_step::desc:
			// No desc names the staged file: it is stale, the caller may take it away.
			if (left) left->push_back(staged);
			return desc_error.empty() ? written.error : desc_error;
		case res::desc_binary_save_step::commit: {
			const res::file_operation back = res::desc_binary::take_back(system, lock, file, written, written_desc, previous);
			// The new desc may be on disk either way: what was loaded of the old one must not stay.
			system.signal_changed(file);
			if (!back) {
				// The desc stays and needs the staged file: both go back together or not at all.
				if (left) {
					left->push_back(staged);
					left->push_back(file);
				}
				return std::format("{}; taking it back failed too, the desc and its staged file stay: {}", written.error, back.error);
			}
			using origin = res::desc_binary::previous_desc::origin;
			const std::string_view done = previous.from == origin::top_layer ? "the previous desc was put back"
				: previous.from == origin::lower_layer ? "the new desc was removed, the one of a lower layer shows again"
				: "the desc was removed";
			return std::format("{}; {}", written.error, done);
		}
		}
	} catch (const std::logic_error& e) {
		// A tag outside the writes the resource system allows.
		return std::format("'{}' may not be written: {}", file.view(), e.what());
	}
	// A store does not evict the cache (the resolver reports its own writes to subscribers only). The desc depends on its
	// binary file, so one signal reaches what was built from either.
	system.signal_changed(file);
	return {};
}

std::string rnd::read_geometry_file(res::resource_system& system, const res::tag& file, const boost::json::object& desc,
	geometry_desc& out)
{
	PROFILE_SCOPE("geometry file read");
	try {
		detail::read_geometry_json(out, desc);
		if (detail::binary_state_of(out) == detail::geometry_binary_state::not_loaded) {
			detail::load_geometry_binary(system, file, out, true);
		}
	} catch (const res::load_error& e) {
		return e.what();
	} catch (const std::exception& e) {
		return std::format("geometry '{}': {}", file.view(), e.what());
	}
	return {};
}

std::string rnd::decode_geometry_files(const res::tag& file, const boost::json::object& desc, std::span<const std::byte> binary,
	geometry_desc& out)
{
	PROFILE_SCOPE("geometry file decode");
	try {
		detail::read_geometry_json(out, desc);
		if (detail::binary_state_of(out) == detail::geometry_binary_state::not_loaded) {
			detail::decode_geometry_binary(file, binary, out);
		} else if (!binary.empty()) {
			return std::format("geometry '{}' holds its data as arrays, but a binary file came with it", file.view());
		} else {
			detail::check_geometry_arrays(file, out);
		}
	} catch (const res::load_error& e) {
		return e.what();
	} catch (const std::exception& e) {
		return std::format("geometry '{}': {}", file.view(), e.what());
	}
	return {};
}

bool rnd::holds_geometry_arrays(const boost::json::object& desc)
{
	const boost::json::value* vertices = desc.if_contains("vertices");
	return vertices != nullptr && vertices->is_array();
}
