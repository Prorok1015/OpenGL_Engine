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
// inline accessors (scene_layer, context_set, module_context, ecs::bus_context...) stays out: a field appended at its end
// moves nothing a module reads and is a minor change, while a reorder or removal there is a major bump by hand, or in
// the window of 4.0 (#758) a new layout_revision. world_context has no inline accessor since #802: its layout is the
// engine's alone. Only sizes and alignments: offsetof is not portable on non-standard-layout types, so a reorder of
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
		// type modules read inline. 1: world_context's accessors out of line (#802). 2: desc_system, schema_registry and
		// dynamic_desc touch their members only out of line (#786). 3: the sandbox's components move into the world by
		// engine code through the module's ecs::sandbox_emplace<T> (#784, #738). 4: field_kind::map and std::optional in
		// schemas (#790): field_kind gains a value and the schema's field table changes for code built before. 5: system_registry
		// is all exports and the world keeps the engine's copy of a component's default (#785, #826). 6: gs::input_actions loses
		// its two warning fields, so the members after them move (#782).
		// 7: inp::action_binding is a {kind, code, threshold} struct, not a variant (#783).
		// 8: desc_base gains the tag of its parent and joins the list (#812); the list now sees its size.
		// 9: field_schema gains `nullable` in the padding after its flags, unseen by its size; it and type_schema, which
		// module code builds too, join the list (#861).
		// 10: cmd::command_desc, which a module builds for gs::game_module::add_command, gains describe_before_allowed_invoke
		// and joins the list (#759).
		// 11: the engine components (scn::engine_component_types), the ctx types and events a module reads, the channel
		// types, app_data_storage and the structs passed by value join the list (#922); no type changes.
		// 12: the ABI revisions of the interfaces with a vtable, the versions of EnTT, Boost and glm and the sizes of
		// entt::registry, boost::json::value and glm::mat4 join the key (#922); no type changes.
		inline constexpr std::uint64_t layout_revision = 12;

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
