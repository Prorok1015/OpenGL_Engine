#pragma once
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/common/engine_assert.h>
#include <filesystem>
#include <functional>
#include <utility>

// Base only (include_base/, not installed in the SDK): pauses a vfs_resolver's write on its io thread, so a test can
// swap a folder or a file for a link between the path check and the write (#414).
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

}
