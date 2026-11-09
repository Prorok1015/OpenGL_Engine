#pragma once
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <optional>
#include <string>

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

	std::string digest_to_hex(const content_digest& digest);
	std::optional<content_digest> digest_from_hex(const std::string& text);

} // namespace edt
