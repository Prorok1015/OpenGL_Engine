#include "edt_data_protocol.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <system_error>
#include <vector>

bool edt::register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path)
{
	std::error_code ec;
	const bool exists = std::filesystem::is_directory(data_path, ec);
	if (!exists) {
		egLOG_WARN("editor/data", "editor data directory '{}' not found: edt:// resources are unavailable", data_path.string());
	}

	auto& resolver = res.registrate_resolver<res::vfs_resolver>(DATA_PROTOCOL, std::vector<std::filesystem::path>{ data_path });
	resolver.set_resource_changed_callback([&res](const res::tag& tag) { res.signal_changed(tag); });
	return exists;
}
