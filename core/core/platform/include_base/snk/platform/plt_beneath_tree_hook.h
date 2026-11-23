#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <snk/common/snk_api.h>
#include <snk/platform/plt_beneath.h>

// Base only (include_base/, not installed in the SDK): remove_tree_beneath with a hook the tests use to change the tree
// while it is walked (#1294).
namespace platform::beneath {

	// Called with each folder of the tree once it is held, before its entries are read.
	using tree_hook = std::function<void(const std::filesystem::path& folder)>;

	// remove_tree_beneath, calling `hook` as it walks.
	SNK_API std::string remove_tree_beneath_watched(pinned_root& root, const std::filesystem::path& folder,
		const std::filesystem::path& leaf, const tree_hook& hook);

}
