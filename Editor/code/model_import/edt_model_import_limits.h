#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// Every limit of the editor's model import (#1044), in one place. A refusal names the limit it hit and its step:
// "<what> exceeds <limit> (<step>)", or "... more than <count> ... (<step>)" for a count.
//
// step           | limit                              | value            | what it limits                                    | enforced in
// source         | k_max_person_source_bytes          | 256 MiB          | the model file a person (or a level) imports      | edt_person_model_import, edt_model_import_core, edt_decoded_model_adapter
// source         | k_max_agent_source_bytes           | 64 MiB           | the model file an agent imports                   | editor_system/import/edt_agent_model_import
// side files     | k_max_side_file_bytes              | 64 MiB           | one file the model reads besides itself           | edt_model_file_access (resolve)
// side files     | k_max_side_files_total_bytes       | 128 MiB          | those files together: listed, read, handed over   | edt_model_file_access (resolve, read), edt_process_decoder (provided_entries)
// side files     | k_max_side_files                   | 64               | how many files the model reads                    | edt_model_file_access (resolve)
// side files     | k_max_names_per_file               | 8                | names one side file is asked for by               | edt_model_file_access, edt_process_decoder
// side files     | k_max_scan_rounds                  | 3                | rounds of the decoder asking for its files        | edt_process_decoder (broker_scan)
// decoder answer | k_max_scan_requests                | 256              | names one scan answer asks for                    | edt_process_decoder (accept_scan_answer)
// decoder answer | k_max_scan_request_name_bytes      | 1024 bytes       | one name a scan answer asks for                   | edt_process_decoder (accept_scan_answer)
// decoder answer | k_decoder_answer_factor x per file | 128 MiB          | one resource the decoder made                     | edt_process_decoder (accept_import_answer), edt_decoded_model_adapter
// decoder answer | k_decoder_answer_factor x total    | 256 MiB          | the decoder's resources together                  | edt_process_decoder (accept_import_answer)
// bundle         | k_max_bundle_manifest_bytes        | 4 MiB            | the manifest (JSON) of a bundle                   | edt_decode_bundle (read_bundle)
// bundle         | k_max_bundle_json_depth            | 32               | nesting of that JSON                              | edt_decode_bundle (read_bundle)
// bundle         | k_max_bundle_blob_bytes            | 256 MiB          | one resource of a bundle                          | edt_decode_bundle (read_bundle)
// bundle         | k_max_bundle_total_bytes           | 512 MiB          | the resources of a bundle together                | edt_decode_bundle (read_bundle)
// bundle         | k_max_bundle_blobs                 | 4096             | how many resources a bundle holds                 | edt_decode_bundle (read_bundle)
// bundle         | k_max_bundle_tag_bytes             | 512 bytes        | the tag of one resource                           | edt_decode_bundle (read_bundle, write_bundle)
// bundle         | largest_bundle_bytes()             | ~518 MiB         | the bundle file; the decoder's --max-output-bytes | edt_decode_bundle (read_bundle), edt_decode_runner
// decoder memory | k_default_decoder_memory_mb        | 2 GiB            | the decoder process; cfg 256..8192 MiB            | edt_process_decoder (decoder_memory_limit), edt_decode_runner
// decoder log    | k_max_decoder_log_lines / _bytes   | 200 lines, 64 KiB| the decoder's stderr in the editor's log          | edt_decode_runner (decode_log_budget)

namespace edt {

	inline constexpr std::string_view k_step_source = "source";
	inline constexpr std::string_view k_step_side_files = "side files";
	inline constexpr std::string_view k_step_decoder_answer = "decoder answer";
	inline constexpr std::string_view k_step_bundle = "bundle";
	inline constexpr std::string_view k_step_decoder_log = "decoder log";

	inline constexpr std::uintmax_t k_max_person_source_bytes = 256ull * 1024ull * 1024ull;
	inline constexpr std::uintmax_t k_max_agent_source_bytes = 64ull * 1024ull * 1024ull;

	inline constexpr std::uintmax_t k_max_side_file_bytes = 64ull * 1024ull * 1024ull;
	inline constexpr std::uintmax_t k_max_side_files_total_bytes = 128ull * 1024ull * 1024ull;
	inline constexpr std::size_t k_max_side_files = 64;
	// How many names one file may be asked for by: each name hands the decoder process its bytes again.
	inline constexpr std::size_t k_max_names_per_file = 8;
	inline constexpr int k_max_scan_rounds = 3;
	inline constexpr std::size_t k_max_scan_requests = 256;
	inline constexpr std::size_t k_max_scan_request_name_bytes = 1024;

	// A decoded resource may be bigger than the file it came from (an image, a mesh rewritten), never more than twice.
	inline constexpr std::uint64_t k_decoder_answer_factor = 2;

	inline constexpr std::uint64_t k_max_bundle_manifest_bytes = 4ull * 1024ull * 1024ull;
	inline constexpr std::uint64_t k_max_bundle_blob_bytes = 256ull * 1024ull * 1024ull;
	inline constexpr std::uint64_t k_max_bundle_total_bytes = 512ull * 1024ull * 1024ull;
	inline constexpr std::uint64_t k_max_bundle_blobs = 4096;
	inline constexpr std::size_t k_max_bundle_json_depth = 32;
	inline constexpr std::size_t k_max_bundle_tag_bytes = 512;

	inline constexpr std::uint64_t k_default_decoder_memory_mb = 2048;
	inline constexpr std::uint64_t k_min_decoder_memory_mb = 256;
	inline constexpr std::uint64_t k_max_decoder_memory_mb = 8192;

	inline constexpr std::size_t k_max_decoder_log_lines = 200;
	inline constexpr std::size_t k_max_decoder_log_bytes = 64 * 1024;

	// A size limit as a refusal names it: "128 MiB", "64 KiB", "2 GiB"; "100 bytes" when it is no whole unit.
	inline std::string limit_text(std::uint64_t bytes)
	{
		constexpr std::uint64_t kib = 1024ull;
		constexpr std::uint64_t mib = 1024ull * kib;
		constexpr std::uint64_t gib = 1024ull * mib;
		if (bytes != 0 && bytes % gib == 0) return std::format("{} GiB", bytes / gib);
		if (bytes != 0 && bytes % mib == 0) return std::format("{} MiB", bytes / mib);
		if (bytes != 0 && bytes % kib == 0) return std::format("{} KiB", bytes / kib);
		return std::format("{} bytes", bytes);
	}

} // namespace edt
