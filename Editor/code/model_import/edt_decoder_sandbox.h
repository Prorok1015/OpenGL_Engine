#pragma once
#include <optional>
#include <string_view>

namespace edt {

	// What the decoder process does when its OS sandbox (#1043: Landlock + seccomp on Linux) cannot be set up. The child
	// enforces it itself, before it reads the request (`--sandbox best_effort|required`):
	// - best_effort: a sandbox the system does not have is skipped (the answer says so, the editor warns once);
	// - required: a sandbox the system does not have ends the child with k_decoder_sandbox_exit_code.
	// A sandbox the system has but that cannot be set up ends the child with that code in both modes.
	enum class sandbox_mode { best_effort, required };

	inline constexpr int k_decoder_sandbox_exit_code = 4;

	// The child prefixes the one stderr line that says why it refused to run with this.
	inline constexpr std::string_view k_decoder_sandbox_refusal_prefix = "snk-sandbox: ";

	// The mode a run gets: the editor's configured one or the one the import asks for, whichever is stricter.
	constexpr sandbox_mode effective_sandbox_mode(sandbox_mode configured, sandbox_mode requested)
	{
		return configured == sandbox_mode::required || requested == sandbox_mode::required ? sandbox_mode::required : sandbox_mode::best_effort;
	}

	constexpr std::string_view sandbox_mode_name(sandbox_mode mode)
	{
		return mode == sandbox_mode::required ? "required" : "best_effort";
	}

	constexpr std::optional<sandbox_mode> sandbox_mode_from_name(std::string_view name)
	{
		if (name == "best_effort") return sandbox_mode::best_effort;
		if (name == "required") return sandbox_mode::required;
		return std::nullopt;
	}

} // namespace edt
