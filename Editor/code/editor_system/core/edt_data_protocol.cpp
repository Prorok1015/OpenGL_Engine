#include "edt_data_protocol.h"
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_mount_table.h>
#include <system_error>

bool edt::register_data_protocol(res::resource_system& res, const std::filesystem::path& data_path)
{
	std::error_code ec;
	const bool exists = std::filesystem::is_directory(data_path, ec);
	if (!exists) {
		egLOG_WARN("editor/data", "editor data directory '{}' not found: edt:// resources are unavailable", data_path.string());
	}

	res.add_mount(res::mount{ DATA_PROTOCOL, { data_path }, res::mount_access::read_only });
	return exists;
}
