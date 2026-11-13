#pragma once
#include <filesystem>
#include <string>

namespace edt
{
	// File > New Project (#765, EPIC-57 section 6.1): gives the files of a project just copied from a template ids of
	// its own, so two projects made from one template never share an asset id. Under every content root of the
	// project's manifest: a .resource sidecar gets a fresh guid (importer, settings and flags kept), a file without one
	// gets a new sidecar, a desc's root __guid becomes its file's new id, and every {"guid", "path"} reference and root
	// __guid in the descs that names an old id is rewritten to the new one. Hidden entries and links are left alone; a
	// desc that is not valid JSON is left as it is with a warning. Returns the error (naming the file), empty on
	// success. Meant for prj::create_project's `prepare`, which runs it on the staging folder.
	std::string give_template_fresh_ids(const std::filesystem::path& project_root);
}
