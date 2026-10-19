#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_bind_group.h>

#include <string>

namespace rnd
{
	// Replaces SNK_UBO(set, slot), SNK_SSBO(set, slot) and SNK_TEX(set, slot) in GLSL source with the layout
	// qualifier that gives the resource the binding the backend's table assigns it:
	//   SNK_UBO(0, 1)  ->  layout(std140, binding = 1)
	//   SNK_SSBO(3, 0) ->  layout(std430, binding = 0)
	//   SNK_TEX(1, 0)  ->  layout(binding = 0)
	// Macros in // comments are left alone. Returns what could not be expanded (a malformed macro, a set or
	// slot outside the table), one problem per line; empty when everything was. The text of such a macro stays,
	// so the shader fails to compile with the macro name in the message.
	SNK_API std::string expand_binding_macros(std::string& source, const driver::binding_table& table);
}
