#pragma once
#include <snk/common/snk_api.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/scene/scn_component_catalog.h>
#include <cstddef>
#include <cstdint>

// The layout key of a module binary (#614): a module built against headers in which one of these types has another
// size or alignment is refused by the host before it is created ("rebuild the project"). The list holds the types
// whose size is compiled into module code: a module constructs, copies or allocates them through engine templates
// (MODULE_LAYER.md, rule next to SNK_MODULE_API_VERSION). A type the engine creates and a module only reads through
// inline accessors (scene_layer, world_context, context_set, module_context...) stays out: a field appended at its end
// moves nothing a module reads and is a minor change, while a reorder or removal there is a major bump by hand.
// Only sizes and alignments: offsetof is not portable on non-standard-layout types, so a reorder of fields of one size
// is not caught. A new type, a comment or a rebuilt engine leave the key as it is.
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

		template<class... Types>
		constexpr std::uint64_t layout_of() noexcept
		{
			std::uint64_t hash = 0xcbf29ce484222325ull;
			((hash = mix_layout(mix_layout(hash, sizeof(Types)), alignof(Types))), ...);
			return hash;
		}
	}

	// The one list of the key: evaluated in whichever binary includes this header.
	constexpr std::uint64_t module_layout_key() noexcept
	{
		return detail::layout_of<
			// register_system builds the entry and the registry's map node in module code.
			ecs::system_desc, ecs::system_instance, ecs::system_entry,
			// bus_type_impl<T> makes the slot of an event type of the module; writers and readers are its system arguments.
			ecs::detail::bus_slot_impl<int>, ecs::event_writer<int>, ecs::event_reader<int>,
			// require<T> and make_dep<T> allocate the control block and the dependency in module code.
			core::res::res_control_block_base, res::res_dep,
			// component_catalog::add<T> builds the entry in module code.
			scn::component_entry>();
	}

	// module_layout_key() as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0 (not checked).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key())
