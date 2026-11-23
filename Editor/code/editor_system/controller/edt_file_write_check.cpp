#include "edt_file_write_check.h"
#include <format>

edt::file_write_check edt::check_file_write(res::resource_system& reader, const desc::desc_system& descs, const desc_file_layout& files,
	const res::tag& tag, std::string_view disk_version)
{
	file_write_check out;
	const desc_file_read on_disk = read_desc_file(reader, descs, files, tag);
	out.version = on_disk.version;
	if (!on_disk.error.empty() && !on_disk.located) {
		out.error = std::format("{} cannot be checked before it is written: {}", tag.view(), on_disk.error);
	} else if (!on_disk.top_layer) {
		out.error = std::format("{} belongs to a lower layer (the engine or a package), not to the project's own res/: nothing was written", tag.view());
	} else if (on_disk.in_open_level) {
		out.error = std::format("{} belongs to the open level now: nothing was written", tag.view());
	} else if (on_disk.version != disk_version) {
		out.changed_on_disk = true;
		out.error = std::format("{} changed on disk since it was opened: nothing was written", tag.view());
	}
	return out;
}
