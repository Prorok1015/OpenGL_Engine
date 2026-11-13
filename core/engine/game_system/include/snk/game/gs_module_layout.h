#pragma once
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/ecs/ecs_system_access.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/game/gs_input_stack_handle.h>
#include <snk/input/inp_input_stack.h>
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
// inline accessors (scene_layer, context_set, module_context, ecs::bus_context...) stays out: a field appended at its end
// moves nothing a module reads and is a minor change, while a reorder or removal there is a major bump by hand, or in
// the window of 4.0 (#758) a new layout_revision. world_context has no inline accessor since #802: its layout is the
// engine's alone. Only sizes and alignments: offsetof is not portable on non-standard-layout types, so a reorder of
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

		// Raised when a layout the list cannot see changes for module code: in the window of 4.0 (#758) a reorder of a
		// type modules read inline. 1: world_context's accessors out of line (#802). 2: desc_system, schema_registry and
		// dynamic_desc touch their members only out of line (#786). 3: the sandbox's components move into the world by
		// engine code through the module's ecs::sandbox_emplace<T> (#784, #738). 4: field_kind::map and std::optional in
		// schemas (#790): field_kind gains a value and the schema's field table changes for code built before.
		inline constexpr std::uint64_t layout_revision = 5;

		template<class... Types>
		constexpr std::uint64_t layout_of() noexcept
		{
			std::uint64_t hash = mix_layout(0xcbf29ce484222325ull, layout_revision);
			((hash = mix_layout(mix_layout(hash, sizeof(Types)), alignof(Types))), ...);
			return hash;
		}
	}

	// The one list of the key: evaluated in whichever binary includes this header.
	constexpr std::uint64_t module_layout_key() noexcept
	{
		return detail::layout_of<
			// register_system builds the parts of an entry in module code: the desc, the access list and the instance its
			// factory makes. The entry and the registry's map node are the engine's (system_registry::add, #785).
			ecs::system_desc, ecs::system_instance, ecs::access_entry,
			// bus_type_impl<T> makes the slot of an event type of the module; writers and readers are its system
			// arguments. Their members do not depend on T, so <int> stands for every T.
			ecs::detail::bus_slot_impl<int>, ecs::event_writer<int>, ecs::event_reader<int>,
			// The engine makes every resource block (pin_block, #758), but the handle reads it inline (is_ready, has_error);
			// require<T> builds the handle the module keeps (in components the engine reads), make_dep<T> the dependency.
			// The handle's members do not depend on T.
			core::res::res_resource_block, core::res::res_handle<int>, res::res_dep,
			// component_catalog::add<T> builds the entry, desc::bind_described<T> the schema binding and its field ops.
			scn::component_entry, desc::schema_binding, desc::field_ops,
			// A module reads a component without a C++ type through find_schema_component and may build one to hand to
			// emplace_schema_component (a const reference: the world keeps the engine's copy, #785).
			desc::dyn_blob,
			// module_context::subscribe/observe build the filter, the slot and the connection the engine keeps.
			scn::event_filter, scn::observer_slot, ecs::connection,
			// module_context::run takes the task's description from module code.
			ds::task_desc,
			// module_context::input_stack hands the module an input_stack_handle by value; the module builds the layers it
			// passes (stack_layer_desc), and a system that takes input_stack_control& gets it made in module code by
			// ecs_invoker's reg.ctx().emplace<T>(), so the module allocates and destroys it.
			gs::input_stack_handle, inp::stack_layer_desc, inp::input_stack_control>();
	}

	// module_layout_key() as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0 (not checked).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key())
