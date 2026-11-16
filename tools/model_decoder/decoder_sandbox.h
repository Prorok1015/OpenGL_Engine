#pragma once
#include "edt_decoder_sandbox.h"
#include <cstdint>
#include <string>

// The decoder's OS sandbox (#1043): what the child takes away from itself before it reads anything of the request.
// Linux: every descriptor but 0..2 closed, RLIMIT_FSIZE, Landlock (the run folder is the only file hierarchy it may touch,
// no TCP, no signals or abstract sockets outside it) and a seccomp allowlist. Other systems have none yet (#1054).
namespace decoder_sandbox {

	struct settings
	{
		edt::sandbox_mode mode = edt::sandbox_mode::best_effort;
		// RLIMIT_FSIZE: no file the child writes may be bigger than the largest answer the editor reads.
		std::uint64_t max_output_bytes = 1ull << 30;
#if defined(SNK_MODEL_DECODER_SELF_TEST)
		// The Landlock probe reports it absent: the tests of the policy on a kernel that has it.
		bool pretend_no_landlock = false;
#endif
	};

	struct status
	{
		bool fs = false;			// Landlock restricts the file system
		bool syscalls = false;		// the seccomp allowlist is installed
		// What is missing and why, for the report when the run goes on without it (best_effort).
		std::string why;
		// Set: the child must not run (exit edt::k_decoder_sandbox_exit_code with this line on stderr).
		std::string refusal;
	};

	// Call once, from the only thread, with the run folder as the working directory, after harden_process.
	status apply(const settings& wanted);

	// "fs=landlock syscalls=seccomp", "fs=none syscalls=none", ...
	std::string report_line(const status& applied);

	// The line the child writes on stderr as soon as the sandbox is applied, before it reads the request: the editor's
	// only word on the sandbox, since anything the child says after the parse may come from a model that took it over.
	// k_decoder_sandbox_status_prefix + report_line + " why=<text>" when something is missing.
	std::string status_line(const status& applied);

} // namespace decoder_sandbox
