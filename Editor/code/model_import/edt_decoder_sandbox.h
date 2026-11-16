#pragma once
#include <optional>
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
