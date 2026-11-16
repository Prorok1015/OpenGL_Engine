#pragma once
#include "edt_decoder_sandbox.h"
#include <snk/editor/edt_problem_list.h>
#include <optional>
#include <string_view>

namespace edt {

	// The editor's word on the model decoder's OS sandbox for this run (#1076): the worst gap a decoder reported, one
	// warning in Problems under `source` until the editor restarts (the setting is read once, at start). The owner's rule:
	// a decoder without its sandbox never fails an import, but a person sees it. Main thread only, like problem_list.
	class decoder_sandbox_notice
	{
	public:
		static constexpr std::string_view source = "decoder";

		// `expected`: what the editor can tell before any run (expected_decoder_sandbox). Shown at once only when the
		// person turned the sandbox off: otherwise the first decoder's own status decides.
		decoder_sandbox_notice(problem_list& problems, sandbox_mode mode, decoder_sandbox_status expected);

		// A decoder's status: a larger gap than any before replaces the row.
		void seen(const decoder_sandbox_status& status);
		// For a question shown before the next run: the worst a decoder reported, else what was expected.
		const decoder_sandbox_status& current() const;

	private:
		void publish(const decoder_sandbox_status& status);

		problem_list& m_problems;
		decoder_sandbox_status m_expected;
		std::optional<decoder_sandbox_status> m_seen;
	};

} // namespace edt
