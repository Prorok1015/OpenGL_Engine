#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace edt {

	// The decoder process's OS sandbox (#1043: Landlock + seccomp on Linux). The owner's rule: isolation is a setting only a
	// person controls (cfg `import/decoder_sandbox`), on by default; an import never fails because the system has none.
	// The child enforces the mode itself, before it reads the request (`--sandbox off|best_effort|required`):
	// - off: no sandbox is set up (the person turned it off);
	// - best_effort (the setting "on"): a sandbox the system does not have is skipped, the status line says so and the
	//   editor warns;
	// - required: a sandbox the system does not have ends the child with k_decoder_sandbox_exit_code (CI and tests only).
	// A sandbox the system has but that cannot be set up ends the child with that code unless the mode is off.
	enum class sandbox_mode { off, best_effort, required };

	inline constexpr int k_decoder_sandbox_exit_code = 4;

	// The child prefixes the one stderr line that says why it refused to run with this.
	inline constexpr std::string_view k_decoder_sandbox_refusal_prefix = "snk-sandbox: ";

	// The child's first stderr line once its sandbox is applied, before it reads the request: "fs=landlock|none
	// syscalls=seccomp|none[ why=<text>]". The editor takes only the first line: a later one may come from a model.
	inline constexpr std::string_view k_decoder_sandbox_status_prefix = "snk-sandbox-status: ";

	// What the child said of its OS sandbox (#1043) in its first stderr line, written before it read anything of the request:
	// after the parse the child may be the model's, so nothing it says later counts.
	struct decoder_sandbox_status
	{
		bool reported = false;
		bool fs = false;		// Landlock restricts the files it can reach
		bool syscalls = false;	// the seccomp allowlist is installed
		std::string why;		// what is missing, escaped, at most 256 bytes
	};

	// What the decoder's sandbox will be before any run, as far as the editor can tell (#1076): the setting, the system and,
	// on Linux, Landlock and seccomp probed in the editor's own process. Only for a question shown before the decoder ran;
	// what the child says (decoder_sandbox_status from its status line) is what counts.
	decoder_sandbox_status expected_decoder_sandbox(sandbox_mode mode);

	// A child that ran without saying how it is sandboxed (its first stderr line was no status): counted as running without
	// any sandbox (#1076), never as unknown.
	decoder_sandbox_status unreported_decoder_sandbox();

	// The line a person (Problems) and an agent (its question, its job's result) are shown when the decoder runs without
	// all of its sandbox; empty when it has all of it or nothing was reported.
	std::string decoder_sandbox_warning(const decoder_sandbox_status& status);

	// How much is missing: 0 nothing (or not reported), 1 the system call filter, 2 the file system isolation.
	int decoder_sandbox_gap(const decoder_sandbox_status& status);

	// The mode a run gets: the editor's setting, raised by what the import asks for (import_source::sandbox: off asks for
	// nothing more). Nothing an import asks for lowers the setting.
	constexpr sandbox_mode effective_sandbox_mode(sandbox_mode configured, sandbox_mode requested)
	{
		return static_cast<int>(requested) > static_cast<int>(configured) ? requested : configured;
	}

	constexpr std::string_view sandbox_mode_name(sandbox_mode mode)
	{
		switch (mode) {
		case sandbox_mode::off: return "off";
		case sandbox_mode::required: return "required";
		case sandbox_mode::best_effort: break;
		}
		return "best_effort";
	}

	constexpr std::optional<sandbox_mode> sandbox_mode_from_name(std::string_view name)
	{
		if (name == "off") return sandbox_mode::off;
		if (name == "best_effort") return sandbox_mode::best_effort;
		if (name == "required") return sandbox_mode::required;
		return std::nullopt;
	}

} // namespace edt
