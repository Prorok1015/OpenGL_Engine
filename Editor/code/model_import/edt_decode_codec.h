#pragma once
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace edt {

	// The JSON the decoder process (#647) and the editor exchange: the files the editor hands over (#1043), what a scan
	// asks for, and what an import produced. Pure, so both sides use the same code; every reader checks types and ranges,
	// because it reads what another process wrote.

	boost::json::object to_json(const referenced_file& file);
	// Empty when `value` is no file entry of this form.
	std::optional<referenced_file> referenced_file_from_json(const boost::json::value& value);

	boost::json::object to_json(const model_file_scan& scan);
	std::optional<model_file_scan> model_file_scan_from_json(const boost::json::value& value);

	// The answer of a scan without its "kind": {"requests":[{"name","kind"}]} or {"error"}. Other keys are ignored.
	boost::json::object to_json(const scan_requests& requests);
	std::optional<scan_requests> scan_requests_from_json(const boost::json::value& value);

	// The fields of an import that are not resources; the resources of `created_tags` travel as bundle blobs.
	boost::json::object to_json(const import_intermediate& imported);
	std::optional<import_intermediate> import_intermediate_from_json(const boost::json::value& value);

	// What the editor asks of the decoder process. The bytes of the model file travel as a bundle blob named
	// k_decode_source_tag, not in the JSON: the editor already read and verified them. So do the bytes of every file it
	// hands over, as provided_blob_tag(<index in decode_request::provided>).
	inline constexpr const char* k_decode_source_tag = "memory://decode_input/source";
	inline constexpr const char* k_decode_file_tag_prefix = "memory://decode_input/file/";

	inline std::string provided_blob_tag(std::size_t index)
	{
		return k_decode_file_tag_prefix + std::to_string(index);
	}

	// A file handed to the decoder (provided_file without its bytes). `present` false: the file is not on the disk, and
	// `relative` is empty.
	struct provided_entry
	{
		std::string name;
		std::string relative;
		model_file_kind kind = model_file_kind::texture;
		bool present = false;
	};

	struct decode_request
	{
		enum class mode_kind { scan, import };
		mode_kind mode = mode_kind::scan;
		std::filesystem::path source_path;
		// The files the editor read by its rule and hands over (#1043); the child opens no other file.
		std::vector<provided_entry> provided;
		model_file_limits limits;
		// Zero: none. The child's own deadline for the parse; the editor also kills it at the same time.
		std::uint64_t deadline_ms = 0;
		// Zero: none. The child's address space limit on POSIX; the Windows job limit is set by the editor.
		std::uint64_t memory_limit_bytes = 0;
		// Empty: the model's folder. The folder side files may come from (import_source::boundary, #925).
		std::filesystem::path boundary;
		// Empty: "<stem>/". The memory:// folder of the import (import_source::memory_folder).
		std::string memory_folder;
		// import_source::on_disk.
		bool on_disk = true;
	};

	boost::json::object to_json(const decode_request& request);
	std::optional<decode_request> decode_request_from_json(const boost::json::value& value);

	std::string digest_to_hex(const content_digest& digest);
	std::optional<content_digest> digest_from_hex(const std::string& text);

} // namespace edt
