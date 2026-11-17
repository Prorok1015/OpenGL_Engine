#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_bind_group.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace rnd
{
	// Replaces SNK_UBO(set, slot), SNK_SSBO(set, slot) and SNK_TEX(set, slot) in GLSL source with the layout
	// qualifier that gives the resource the binding the backend's table assigns it:
	//   SNK_UBO(0, 1)  ->  layout(std140, binding = 1)
	//   SNK_SSBO(3, 0) ->  layout(std430, binding = 0)
	//   SNK_TEX(1, 0)  ->  layout(binding = 0)
	// Macros in // and /* */ comments are left alone. Returns what could not be expanded, one problem per line
	// ("line N: ..."): a malformed macro, a set or slot outside the table, or another name used like a macro
	// (SNK_ is the engine's prefix). Empty when everything was. The text of such a macro stays, and the caller
	// has to refuse the shader: the compiler would only see a call it cannot resolve.
	SNK_API std::string expand_binding_macros(std::string& source, const driver::binding_table& table);

	// Puts `text` on the lines after the #version directive, which has to be the first thing in the source that
	// is not a comment or blank. A UTF-8 byte order mark in front of it is removed. False (and `source` unchanged,
	// the mark apart) when the directive is not first.
	SNK_API bool insert_after_version(std::string& source, std::string_view text);

	// The source with every comment character turned into a space and the newlines kept, so offsets and line
	// numbers stay the same: what is left is the text the compiler reads.
	SNK_API std::string blank_comments(const std::string& source);
}
