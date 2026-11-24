#pragma once
#include <filesystem>
#include <functional>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_beneath.h>

// Base only (include_base/, not installed in the SDK): what the tests of #1304 change in hold_folder_beneath.
namespace platform::beneath {

	// Called by hold_folder_beneath with the folder below the root once everything is held, before it returns.
	using hold_hook = std::function<void(const std::filesystem::path& folder)>;

	// Sets the hook of every later hold; an empty one takes it away.
	SNK_API void set_hold_hook(hold_hook hook);
	// true: a hold on Linux gives no load path, as where /proc/self/fd cannot be used; false: as found.
	SNK_API void set_proc_fd_unusable(bool unusable);

}
