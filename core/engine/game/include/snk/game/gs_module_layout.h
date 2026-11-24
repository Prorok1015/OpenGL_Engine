#pragma once
#include <snk/common/snk_api.h>
#include <cstdint>
#include <type_traits>

namespace scn
{
	template<class...>
	struct type_list;
}

// The layout key of a module binary (#614): a module built against headers in which one of these types has another
// size or alignment is refused by the host before it is created ("rebuild the project"). The list holds the types
// whose size is compiled into module code: a module constructs, copies or allocates them through engine templates
// (MODULE_LAYER.md, rule next to SNK_MODULE_API_VERSION), walks their storage (the engine components) or reads them
// whole from registry.ctx() and the bus (#922). A type the engine creates and a module only reads through
// inline accessors (context_set, module_context, ecs::bus_context...) stays out: a field appended at its end
// moves nothing a module reads and is a minor change, while a reorder or removal there is a major bump by hand, or in
// the window of 4.0 (#758) a new layout_revision. world_context (#802), scene_layer and input_actions (#922 PR 3) have
// no inline accessor: their layout is the engine's alone. Only sizes and alignments: offsetof is not portable on non-standard-layout types, so a reorder of
// fields of one size is not caught. A vtable a module calls or fills enters as the <interface>_abi_revision declared
// next to it, which tools/ci/check_vtable_pins.py holds to its bases and virtual functions; the versions of EnTT,
// Boost and glm and the sizes of their types enter too (#922). A new type, a comment or a rebuilt engine leave the
// key as it is. Debug and Release differ only where the standard library does
// (MSVC's _ITERATOR_DEBUG_LEVEL); such a mix is refused earlier anyway, by the build flags.
// The list itself and the headers of its types are in gs_module_layout_key.h, which only the file that declares a module
// and gs_module_layout.cpp compile: every other file of a module that includes gs_game_module.h stays without them.
namespace gs
{
	namespace detail
	{
		constexpr std::uint64_t mix_layout(std::uint64_t hash, std::uint64_t value) noexcept
		{
			for (int byte = 0; byte < 8; ++byte) {
				hash ^= (value >> (byte * 8)) & 0xffu;
				hash *= 0x100000001b3ull;
			}
			return hash;
		}

		// Raised when a layout the list cannot see changes for module code: in the window of 4.0 (#758) a reorder of a
		// type modules read inline, or a type of the list changes. Each value is told in docs/abi (HISTORY.md, changes/).
		inline constexpr std::uint64_t layout_revision = 26;

		// Numbers the key mixes in as they are, in order: the interfaces' ABI revisions and the third-party versions.
		template<std::uint64_t... Values>
		struct layout_values
		{
		};

		template<class T>
		struct layout_part
		{
			static constexpr std::uint64_t mix(std::uint64_t hash) noexcept
			{
				return mix_layout(mix_layout(hash, sizeof(T)), alignof(T));
			}
		};

		// A scn::type_list in the list stands for its types, in order: the key is the same as with them written out.
		template<class... Types>
		struct layout_part<scn::type_list<Types...>>
		{
			static constexpr std::uint64_t mix(std::uint64_t hash) noexcept
			{
				((hash = layout_part<Types>::mix(hash)), ...);
				return hash;
			}
		};

		template<std::uint64_t... Values>
		struct layout_part<layout_values<Values...>>
		{
			static constexpr std::uint64_t mix(std::uint64_t hash) noexcept
			{
				((hash = mix_layout(hash, Values)), ...);
				return hash;
			}
		};

		template<class... Types>
		constexpr std::uint64_t layout_of() noexcept
		{
			std::uint64_t hash = mix_layout(0xcbf29ce484222325ull, layout_revision);
			((hash = layout_part<Types>::mix(hash)), ...);
			return hash;
		}
	}

	namespace detail
	{
		// The key of the list, as gs_module_layout_key.h specializes it. A file that declares a module without that header
		// gets this error instead of a key of 0, which a host refuses only of a module in a library.
		template<class List = void>
		struct listed_layout
		{
			static_assert(!std::is_void_v<List>,
				"SNK_DECLARE_MODULE needs the module layout key: #include <snk/game/gs_module_layout_key.h> in this file, "
				"before the macro");
		};
	}

	// The key of gs_module_layout_key.h as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0, which a host
// refuses of a module in a library (app::modules::check_library_module).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::detail::listed_layout<>::key)
