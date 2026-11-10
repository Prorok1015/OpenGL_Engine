#pragma once
#include <snk/common/snk_api.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_scene_layer.h>
#include <cstddef>
#include <cstdint>

// The layout key of a module binary (#614): a module built against headers in which one of these types has another
// size or alignment is refused by the host before it is created ("rebuild the project"), since the module allocates
// them or runs their inline code (MODULE_LAYER.md, rule next to SNK_MODULE_API_VERSION). Only sizes and alignments:
// offsetof is not portable on non-standard-layout types, so a reorder of fields of one size is not caught.
// Add a type here when a module creates it or runs its inline code; a new type, a comment or a rebuilt engine leave
// the key as it is, so a module built for an older minor still loads.
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
			ecs::system_desc, ecs::system_instance, ecs::system_entry,
			ecs::detail::bus_state, ecs::detail::bus_slot_impl<int>, ecs::event_writer<int>, ecs::event_reader<int>,
			core::res::res_control_block_base, res::res_dep,
			scn::component_entry, scn::scene_layer, scn::world_desc, scn::world_context, scn::context_set,
			scn::context_manager>();
	}

	// module_layout_key() as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0 (not checked).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key())
