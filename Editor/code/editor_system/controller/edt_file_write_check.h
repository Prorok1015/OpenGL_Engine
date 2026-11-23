#pragma once
#include "edt_desc_file_commands.h"
#include <snk/model_codec/res_tag.h>
#include <string>
#include <string_view>

namespace edt
{
	// What a document that edits a desc file asks before it writes over it (the prefab editor's Apply, the mesh document's Save):
	// the file is read as it is now and must be one of the project's own top layer, must not belong to the open level and must be
	// the one the document read (`disk_version`, as code_version_of gives it). Nothing is written.
	struct file_write_check
	{
		// Empty when the file may be written; else why not, ready to show.
		std::string error;
		// The file differs from `disk_version`: someone else wrote it since the document read it.
		bool changed_on_disk = false;
		// code_version_of the file's bytes now.
		std::string version;
	};

	file_write_check check_file_write(res::resource_system& reader, const desc::desc_system& descs, const desc_file_layout& files,
		const res::tag& tag, std::string_view disk_version);
}
