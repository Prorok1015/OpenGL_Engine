#pragma once
#include <snk/common/snk_api.h>
#include <snk/resource/res_tag.h>

#include <string>
#include <vector>

namespace res { class resource_system; }

namespace rnd
{
	// Replaces every `#include "name"` line of a shader source with the text of the file, through the resource
	// system: `name` is resolved next to the file that includes it (the same protocol and directory), so nested
	// includes work from any folder of the VFS. `origin` is the tag the source was read from.
	//
	// Guards: a file is expanded once per call, wherever and however often it is included, as if it began with
	// `#pragma once`. The second include of a file leaves nothing in its place, so declarations shared by several
	// libraries (snk_common.glsl) never collide.
	//
	// A file counts as expanded on its first include even inside an inactive #if / #ifdef, so a later include of
	// the same file is dropped: stricter than the compiler's #pragma once. Nesting is limited to 32 files.
	//
	// Includes in comments are left alone. Returns what could not be expanded, one problem per line
	// ("'file' line N: ..."): a file that cannot be read, an include that closes a cycle (the chain is named, the
	// recursion stops there), an include that is not written as "name". Empty when everything was.
	//
	// `files`, when given, receives the tag of every file the expansion asked for, each once, in the order it was
	// reached: the ones that could not be read as well, so the caller learns when one of them appears or changes.
	SNK_API std::string expand_includes(std::string& source, const res::tag& origin, res::resource_system& res_sys, std::vector<res::tag>* files = nullptr);
}
