#pragma once
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace edt {

	// The JSON the decoder process (#647) and the editor exchange: the files a model may read (what a person was
	// shown, with their identity and digest), what a scan found, and what an import produced. Pure, so both sides use
	// the same code; every reader checks types and ranges, because it reads what another process wrote.

	boost::json::object to_json(const referenced_file& file);
	// Empty when `value` is no file entry of this form.
	std::optional<referenced_file> referenced_file_from_json(const boost::json::value& value);

	boost::json::object to_json(const model_file_scan& scan);
	std::optional<model_file_scan> model_file_scan_from_json(const boost::json::value& value);

	// The fields of an import that are not resources; the resources of `created_tags` travel as bundle blobs.
	boost::json::object to_json(const import_intermediate& imported);
	std::optional<import_intermediate> import_intermediate_from_json(const boost::json::value& value);

	// What the editor asks of the decoder process. The bytes of the model file travel as a bundle blob named
	// k_decode_source_tag, not in the JSON: the editor already read and verified them.
	inline constexpr const char* k_decode_source_tag = "memory://decode_input/source";

	struct decode_request
	{
		enum class mode_kind { scan, import };
		mode_kind mode = mode_kind::scan;
		std::filesystem::path source_path;
		// Import only: what the person was shown; the child re-verifies every side file against it.
		std::vector<referenced_file> allowed;
		model_file_limits limits;
		// Zero: none. The child's own deadline for the parse; the editor also kills it at the same time.
		std::uint64_t deadline_ms = 0;
		// Zero: none. The child's address space limit on POSIX; the Windows job limit is set by the editor.
		std::uint64_t memory_limit_bytes = 0;
	};

	boost::json::object to_json(const decode_request& request);
	std::optional<decode_request> decode_request_from_json(const boost::json::value& value);

	std::string digest_to_hex(const content_digest& digest);
	std::optional<content_digest> digest_from_hex(const std::string& text);

} // namespace edt
