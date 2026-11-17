#pragma once
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/common/engine_assert.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>

// Base only (include_base/, not installed in the SDK): pauses a vfs_resolver's write or read on its io thread, so a test
// can swap a folder or a file for a link between the path check and the write (#414) or the read (#484).
namespace res::detail {

	struct write_hook_access {
		// Set before any store and cleared once the stores are done (store_sync returned): the io thread reads the hook
		// without a lock, so it must not change while a write is in flight.
		static void set(vfs_resolver& resolver, std::function<void(write_stage, const std::filesystem::path& tmp_path)> hook)
		{
			ASSERT_MSG(resolver.m_writes_in_flight.load() == 0, "vfs write hook changed while a store is in flight");
			resolver.m_write_hook = std::move(hook);
		}
	};

	struct read_hook_access {
		// Set before the reads it is meant for and cleared after they finished: the io thread reads both without a lock.
		static void set(vfs_resolver& resolver, std::function<void(read_stage)> hook, std::uint64_t max_bytes = 0)
		{
			resolver.m_read_hook = std::move(hook);
			resolver.m_read_limit = max_bytes;
		}
	};

}
