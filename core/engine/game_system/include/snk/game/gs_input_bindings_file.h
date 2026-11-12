#pragma once
#include <snk/input/inp_user_bindings.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <string>
#include <string_view>

// The player's changed bindings on disk (#736):
//   { "__type": "user_input_bindings", "__version": 1,
//     "actions": [ { "name": "MoveLeft", "bindings": [ { "key": "Q" }, { "mouse": "LEFT" } ] } ] }
// Key and button names are those of inp::key_from_name and inp::mouse_button_from_name.
namespace gs
{
	// Reads text as written by write_user_bindings. A binding with an unknown name, or neither or both of "key" and
	// "mouse", is warned about with `source` and skipped; text that is no such document gives no bindings.
	SNK_API inp::user_bindings parse_user_bindings(std::string_view text, std::string_view source = {});
	SNK_API std::string write_user_bindings(const inp::user_bindings& bindings);

	// A file that is not there reads as no bindings without a warning; a link, a folder or a file over 256 KiB at its name
	// is warned about and read as none. At most 256 actions of 16 bindings are read. Main thread.
	SNK_API inp::user_bindings load_user_bindings(const std::filesystem::path& file);
	// prj::write_text_atomically (a temp file next to it, renamed over it, so a crash leaves the old one); false (and a
	// warning) when that failed.
	SNK_API bool save_user_bindings(const std::filesystem::path& file, const inp::user_bindings& bindings);
}
