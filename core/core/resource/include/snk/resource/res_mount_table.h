#pragma once
#include <filesystem>
#include <string_view>
#include <vector>

namespace res
{
	enum class mount_access
	{
		read_write,
		read_only,
	};

	// One protocol served from directories by a vfs_resolver.
	struct mount
	{
		// Must outlive the resource system (a constant such as res::tag::memory): its resolver map is keyed by the view.
		std::string_view protocol;
		// Lowest priority first: the last root wins a lookup and receives writes.
		std::vector<std::filesystem::path> roots;
		mount_access access = mount_access::read_write;
	};

	using mount_table = std::vector<mount>;
}
