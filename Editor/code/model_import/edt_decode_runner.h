#pragma once
#include "edt_decode_bundle.h"
#include "edt_decode_codec.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace edt {

	struct decode_runner_config
	{
		// snk_model_decoder; empty: the one next to the running program.
		std::filesystem::path decoder;
		// The child is killed when it runs longer. Also the deadline the child gets for its parse.
		std::chrono::milliseconds timeout{ std::chrono::minutes(2) };
		std::uint64_t memory_limit_bytes = 2ull * 1024ull * 1024ull * 1024ull;
		bundle_limits answer_limits;
		// Appended to the child's command line; the process tests use snk_model_decoder's --self-test-* flags.
		std::vector<std::string> extra_arguments;
	};

	struct decode_outcome
	{
		// Set when the child ended normally and wrote a readable bundle (its manifest may still say "error").
		std::optional<decode_bundle> bundle;
		// Set when `bundle` is empty: why (child failed, was killed, or wrote what could not be read).
		std::string error;
		bool timed_out = false;
		bool cancelled = false;

		bool ok() const { return bundle.has_value(); }
	};

	// What the child printed is the model's doing: a line made safe for the log. Control characters, C1 controls, the
	// line and paragraph separators and the bidi marks are written as \xNN (each byte of their UTF-8), so a line cannot
	// forge log lines, move a terminal's cursor or reorder the text a person reads.
	std::string decode_log_text(std::string_view line);

	// What a run may put in the editor's log: past 200 lines or 64 KiB one line says so and the rest is dropped (Assimp
	// warns about every odd thing in a model; a hostile one can flood the log for the whole deadline).
	class decode_log_budget
	{
	public:
		// Counts one line; true while it may be logged.
		bool take(std::size_t bytes);

	private:
		static constexpr std::size_t k_max_lines = 200;
		static constexpr std::size_t k_max_bytes = 64 * 1024;
		std::mutex m_mutex;
		std::size_t m_lines = 0;
		std::size_t m_bytes = 0;
		bool m_done = false;
	};

	// Runs the decoder process (#647) for one request. The model's bytes go in the request bundle; the child gets a
	// clean environment and a scratch folder as its working directory, which is deleted on every path, a kill included.
	// Blocks: call it from a worker thread.
	class decode_runner
	{
	public:
		explicit decode_runner(decode_runner_config config);

		decode_outcome run(const decode_request& request, const std::vector<std::byte>& source, std::stop_token stop) const;

		// Deletes the run folders of decoders whose editor process no longer runs (an editor that crashed mid-import):
		// core::res::remove_stale_scratch_dirs, with its rules (only a private directory of this user, never a link).
		// Call it once at startup. Returns how many it removed.
		static std::size_t remove_stale_folders();

	private:
		decode_runner_config m_config;
	};

} // namespace edt
