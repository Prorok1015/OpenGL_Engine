#pragma once
#include "cmd_registry.h"
#include <snk/common/snk_api.h>
#include <functional>
#include <string>
#include <string_view>

namespace cmd
{
	// What one caller gets of the journal. Both are optional: without them every entry goes out as it was logged.
	// The editor asks the same filter for what its other read commands hand out (problems, jobs, dialogs).
	struct log_read_filter
	{
		// false: the entry is left out for this caller; it does not count against `limit`.
		std::function<bool(std::string_view category, const origin& from)> visible;
		// The text this caller gets in place of the entry's.
		std::function<std::string(std::string_view text, const origin& from)> redact;
	};

	// Registers `log.read{since_seq?, min_level?, category?, limit?}` -> `{entries, next_seq, dropped}`, a read of the
	// engine log journal (engine::read_log_journal). An entry is {seq, time_ms (unix, UTC), level, category, text}.
	// `since_seq` is inclusive and 0 or absent means "from the oldest entry held"; pass the previous `next_seq` to
	// continue. `dropped` says that `since_seq` was already pushed out of the ring. `category` matches the category
	// and everything below it ("edt" also gives "edt/import"). `limit` is 1..500, 100 by default.
	// A game without the editor registers it into its own command_registry the same way.
	[[nodiscard]] SNK_API registration register_log_commands(command_registry& registry);

	// The same with a filter. `filter` (may be null; it must outlive the registration) is asked on every read, so
	// whoever owns the callers' rights can set it later: the journal holds what a command took care not to hand its
	// caller (a path on disk, the output of a build an agent may not run), and a read of it must not hand that over
	// either (#935). An overload, not a default argument: the one-argument symbol stays exported.
	[[nodiscard]] SNK_API registration register_log_commands(command_registry& registry, const log_read_filter* filter);
}
