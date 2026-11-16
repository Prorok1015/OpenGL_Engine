#pragma once
#include <snk/game/gs_module_layout.h>
#include <cstdint>
#include <type_traits>

// The editor layout key of a game_editor module (#969): what gs::module_layout_key is to the engine's types, this is to
// the editor's. A module built against editor headers in which one of the listed types has another size or alignment,
// an interface another ABI revision or ImGui another version is refused by the editor before it is created
// (edt::check_editor_api_version: "rebuild the project"). The list holds the editor types a module constructs, copies or
// allocates (descs, handles, signals, the bases of its panels, documents and extension) and the host types whose fields
// its inline code reads (editor_context and what it holds by value). The rules are those of gs_module_layout.h: sizes and
// alignments only; a vtable enters as the <interface>_abi_revision next to it, pinned by tools/ci/check_vtable_pins.py
// with "key": "editor". The list and the headers of its types are in edt_editor_layout_key.h.
namespace edt
{
	namespace detail
	{
		// Raised when a layout the list cannot see changes for module code: a reorder of fields of one size in a listed
		// type, or a field a module reads inline moving inside a host type. 1: the key is introduced (#969).
		inline constexpr std::uint64_t editor_layout_revision = 1;

		// gs::detail::layout_of seeded with editor_layout_revision: the parts mix in as they do in the game key.
		template<class... Types>
		constexpr std::uint64_t editor_layout_of() noexcept
		{
			std::uint64_t hash = gs::detail::mix_layout(0xcbf29ce484222325ull, editor_layout_revision);
			((hash = gs::detail::layout_part<Types>::mix(hash)), ...);
			return hash;
		}

		// The key as edt_editor_layout_key.h specializes it: what SNK_DECLARE_EDITOR_MODULE exports, a constant.
		template<class List = void>
		struct listed_editor_layout
		{
			static_assert(!std::is_void_v<List>,
				"SNK_DECLARE_EDITOR_MODULE needs the editor layout key: #include <snk/editor/edt_editor_layout_key.h>");
		};
	}
}
