#pragma once
#include <snk/platform/plt_reparse_tag.h>

// Base only (include_base/, not installed in the SDK). The names the resource system used before the no-follow file
// primitives moved to platform::beneath (#523).
namespace res::detail {

	using platform::beneath::k_reparse_tag_mount_point;
	using platform::beneath::k_reparse_tag_symlink;
	using platform::beneath::reparse_tag_redirects;

}
