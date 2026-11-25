#pragma once
#include <snk/common/snk_api.h>
#include <snk/model_codec/res_tag.h>

#include <string>
#include <vector>

namespace res { class resource_system; }

namespace rnd
{
	// Replaces every `#include "name"` line of a shader source with the text of the file, through the resource
	// system: `name` is resolved next to the file that includes it (the same protocol and directory), so nested
	// includes work from any folder of the VFS. `origin` is the tag the source was read from.
	//
	// Guards: a file is expanded at every include of it, wrapped in its own guard (`#ifndef SNK_INCLUDE_<hash>` /
	// `#define` ... `#endif`), so the compiler keeps the first copy it takes, as with `#pragma once`, and declarations
	// shared by several libraries (snk_common.glsl) never collide. An include inside a group the compiler skips does
	// not hide the file from a later include in a group it keeps (#1301). Nesting is limited to 32 files, and the
	// expanded text is bounded as well.
	//
	// Includes in comments are left alone. Returns what could not be expanded, one problem per line
	// ("'file' line N: ..."): a file that cannot be read, an include that closes a cycle (the chain is named, the
	// recursion stops there), an include that is not written as "name". Empty when everything was.
	//
	// `files`, when given, receives the tag of every file the expansion asked for, each once, in the order it was
	// reached: the ones that could not be read as well, so the caller learns when one of them appears or changes.
	SNK_API std::string expand_includes(std::string& source, const res::tag& origin, res::resource_system& res_sys, std::vector<res::tag>* files = nullptr);
}
