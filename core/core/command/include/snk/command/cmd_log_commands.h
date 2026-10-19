#pragma once
#include "cmd_registry.h"
#include <snk/common/snk_api.h>

namespace cmd
{
	// Registers `log.read{since_seq?, min_level?, category?, limit?}` -> `{entries, next_seq, dropped}`, a read of the
	// engine log journal (engine::read_log_journal). An entry is {seq, time_ms (unix, UTC), level, category, text}.
	// `since_seq` is inclusive and 0 or absent means "from the oldest entry held"; pass the previous `next_seq` to
	// continue. `dropped` says that `since_seq` was already pushed out of the ring. `category` matches the category
	// and everything below it ("edt" also gives "edt/import"). `limit` is 1..500, 100 by default.
	// A game without the editor registers it into its own command_registry the same way.
	[[nodiscard]] SNK_API registration register_log_commands(command_registry& registry);
}
