#pragma once
#include <snk/input/inp_user_bindings.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace desc { class desc_system; }

// The player's changed bindings on disk (#736):
//   { "__type": "user_input_bindings", "__version": 1,
//     "actions": [ { "name": "MoveLeft", "bindings": [ { "key": "Q" }, { "mouse": "LEFT" } ] } ] }
// Key and button names are those of inp::key_from_name and inp::mouse_button_from_name.
namespace gs
{
	// Reads text as written by write_user_bindings. A binding with an unknown name, or neither or both of "key" and
	// "mouse", is warned about with `source` and skipped; text that is no such document gives no bindings.
	SNK_API inp::user_bindings parse_user_bindings(std::string_view text, std::string_view source = {});

	// What the text of a bindings file turned out to be. newer: a bindings document of a version newer than this engine
	// reads, a newer build's settings that this one must never write over. unreadable: no bindings document (empty, cut
	// short, not JSON, another __type), a malformed __version, or a step that failed.
	enum class user_file_state { usable, newer, unreadable };

	// The same through the migration steps `descs` has for user_input_bindings (EPIC-58 section 7.2): an older file is
	// brought up in memory and left as it is on disk. Unless `state` comes back usable the bindings are none, so the
	// project's stay. Without `descs` only version 1 is read.
	SNK_API inp::user_bindings parse_user_bindings(std::string_view text, std::string_view source, const desc::desc_system* descs,
		user_file_state& state);
	SNK_API std::string write_user_bindings(const inp::user_bindings& bindings);
	// Empty when the version write_user_bindings writes is the one the steps `descs` has for user_input_bindings lead to;
	// else what is wrong (EPIC-58 section 7.2): the player's file would read back as newer and never be saved again, or
	// go through a step it needs not.
	SNK_API std::string user_bindings_version_problem(const desc::desc_system& descs);

	// A file that is not there reads as no bindings without a warning; a link, a folder or a file over 256 KiB at its name
	// is warned about and read as none. At most 256 actions of 16 bindings are read. The file is never moved. Main thread.
	SNK_API inp::user_bindings load_user_bindings(const std::filesystem::path& file);
	// The same through parse_user_bindings with `descs`. `keep_file` is set when the file must not be written over: it is
	// newer, it cannot be read from disk (a link, a folder, over 256 KiB), or it is unreadable and could not be moved
	// aside. An unreadable one is renamed to <name>.broken-<UTC time> next to it, with a warning naming it, so the next
	// save writes a new file and one bad write never stops the player's bindings from being saved.
	SNK_API inp::user_bindings load_user_bindings(const std::filesystem::path& file, const desc::desc_system* descs, bool& keep_file);
	// prj::write_project_file with the file's folder below its parent: the folder and the file are opened without following
	// a link (a link at the name is an error and its target stays), the new text goes to a temp file renamed over it, so a
	// crash leaves the old one. False (and a warning) when that failed.
	SNK_API bool save_user_bindings(const std::filesystem::path& file, const inp::user_bindings& bindings);
}
