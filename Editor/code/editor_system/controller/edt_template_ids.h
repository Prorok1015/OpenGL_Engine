#pragma once
#include <filesystem>
#include <string>

namespace edt
{
	// File > New Project (#765, EPIC-57 section 6.1): gives the files of a project just copied from a template ids of
	// its own, so two projects made from one template never share an asset id. Under every content root of the
	// project's manifest: a .resource sidecar gets a fresh guid (importer, settings and flags kept), a file without one
	// (found in any letter case) gets a new sidecar, the root __guid of every desc becomes the new id of its own sidecar,
	// and every {"guid", "path"} reference naming an old id is rewritten to the new id of the first file that had it.
	// Hidden folders, ignored names (res::is_ignored_asset_name) and links are left alone; a content root reached through
	// a link, a malformed sidecar, a desc that is not JSON or is over 4 MiB fail it. Reads and writes never follow a
	// link below the root (prj::read_project_file / write_project_file). Returns the error (naming the file), empty on
	// success. Meant for prj::create_project's `prepare`, which runs it on the staging folder.
	std::string give_template_fresh_ids(const std::filesystem::path& project_root);
}
