#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/ecs/ecs_system_access.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <cstddef>
#include <cstdint>

// The layout key of a module binary (#614): a module built against headers in which one of these types has another
// size or alignment is refused by the host before it is created ("rebuild the project"). The list holds the types
// whose size is compiled into module code: a module constructs, copies or allocates them through engine templates
// (MODULE_LAYER.md, rule next to SNK_MODULE_API_VERSION). A type the engine creates and a module only reads through
// inline accessors (scene_layer, world_context, context_set, module_context, ecs::bus_context...) stays out: a field
// appended at its end moves nothing a module reads and is a minor change, while a reorder or removal there is a major
// bump by hand. Only sizes and alignments: offsetof is not portable on non-standard-layout types, so a reorder of
// fields of one size is not caught, nor a change of a vtable (bus_slot, bus_type, module_interface). A new type, a
// comment or a rebuilt engine leave the key as it is. Debug and Release differ only where the standard library does
// (MSVC's _ITERATOR_DEBUG_LEVEL); such a mix is refused earlier anyway, by the build flags.
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
			// register_system builds the entry, its access list and the registry's map node in module code.
			ecs::system_desc, ecs::system_instance, ecs::system_entry, ecs::access_entry,
			// bus_type_impl<T> makes the slot of an event type of the module; writers and readers are its system
			// arguments. Their members do not depend on T, so <int> stands for every T.
			ecs::detail::bus_slot_impl<int>, ecs::event_writer<int>, ecs::event_reader<int>,
			// pin_resource<T> makes the resource block in module code; require<T> builds the handle the module keeps (in
			// components the engine reads), make_dep<T> the dependency. The handle's members do not depend on T.
			core::res::res_resource_block, core::res::res_handle<int>, res::res_dep,
			// component_catalog::add<T> builds the entry, desc::bind_described<T> the schema binding and its field ops.
			scn::component_entry, desc::schema_binding, desc::field_ops,
			// module_context::subscribe/observe build the filter, the slot and the connection the engine keeps.
			scn::event_filter, scn::observer_slot, ecs::connection,
			// module_context::run takes the task's description from module code.
			ds::task_desc>();
	}

	// module_layout_key() as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0 (not checked).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key())
