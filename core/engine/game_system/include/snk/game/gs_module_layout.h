#pragma once
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/ecs/ecs_system_access.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/game/gs_input_stack_handle.h>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_stack.h>
#include <snk/input/inp_user_bindings.h>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/scn_animation.h>
#include <snk/scene/scn_blackboard_catalog.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_engine_component_list.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <snk/scene/scn_time.h>
#include <cstddef>
#include <cstdint>

// The layout key of a module binary (#614): a module built against headers in which one of these types has another
// size or alignment is refused by the host before it is created ("rebuild the project"). The list holds the types
// whose size is compiled into module code: a module constructs, copies or allocates them through engine templates
// (MODULE_LAYER.md, rule next to SNK_MODULE_API_VERSION), walks their storage (the engine components) or reads them
// whole from registry.ctx() and the bus (#922). A type the engine creates and a module only reads through
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
		inline constexpr std::uint64_t layout_revision = 11;

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

		template<class... Types>
		constexpr std::uint64_t layout_of() noexcept
		{
			std::uint64_t hash = mix_layout(0xcbf29ce484222325ull, layout_revision);
			((hash = layout_part<Types>::mix(hash)), ...);
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
			// component_catalog::add<T> builds the entry, desc::bind_described<T> the schema binding, its field ops and its
			// schema with the fields (described_schema<T>).
			scn::component_entry, desc::schema_binding, desc::field_ops, desc::type_schema, desc::field_schema,
			// A module reads a component without a C++ type through find_schema_component and may build one to hand to
			// emplace_schema_component (a const reference: the world keeps the engine's copy, #785).
			desc::dyn_blob,
			// A module's desc types derive from desc_base (schema_desc<T>) and are created by its factories.
			desc::desc_base,
			// module_context::subscribe/observe build the filter, the slot and the connection the engine keeps.
			scn::event_filter, scn::observer_slot, ecs::connection,
			// module_context::run takes the task's description from module code.
			ds::task_desc,
			// module_context::input_stack hands the module an input_stack_handle by value; the module builds the layers it
			// passes (stack_layer_desc), and a system that takes input_stack_control& gets it made in module code by
			// ecs_invoker's reg.ctx().emplace<T>(), so the module allocates and destroys it.
			gs::input_stack_handle, inp::stack_layer_desc, inp::input_stack_control, inp::action_binding,
			// game_module::add_command takes the command a module describes.
			cmd::command_desc,
			// A module's systems walk view<T>/storage<T> of the engine's components inline, a step of sizeof(T) each.
			scn::engine_component_types,
			// ctx resources a module's system takes by reference from registry.ctx() and reads or writes inline.
			scn::delta_time, scn::fixed_time, scn::update_alpha, scn::world_time, ecs::rng, ecs::entity_changer,
			ecs::entity_spawner, inp::action_state, inp::fixed_action_state, inp::action_map_ref, inp::action_trace,
			inp::input_capture, scn::node_index,
			// Engine events a module's event_reader<T> copies out of the bus.
			inp::action_event, scn::animation_finished,
			// All inline: module code runs find_entry over its members.
			ds::app_data_storage,
			// register_desc_spawner builds the pair and inserts it into the assembler's map in module code;
			// component_registry is inline too.
			scn::ecs_assembler, scn::ecs_assembler::spawner_pair, ecs::component_registry,
			// A module sends and reads set mail through outbox<T>/inbox<T>; channel_type_impl<T>::make_slot makes the
			// slot of its message type in module code. <int> stands for every T, whose own size the key does not see.
			ecs::address, ecs::entity_ref, ecs::envelope<int>, ecs::outbox<int>, ecs::inbox<int>,
			ecs::detail::channel_slot_impl<int>,
			// Passed by value between a module and the engine's exports or callbacks.
			desc::override_failure, desc::read_context, cmd::call, cmd::result, desc::enum_item, desc::enum_schema,
			scn::blackboard_type,
			// task_system::run makes the task's state in module code and then_main hands its callback the result.
			ds::task_result<void>, ds::detail::task_state<void>,
			// gs::input_actions' inline accessors copy or hand out these members.
			inp::gamepad_config, inp::user_bindings>();
	}

	// module_layout_key() as the engine was built: what the host compares a module's key with.
	SNK_API std::uint64_t engine_module_layout_key() noexcept;
}

// SNK_DECLARE_MODULE exports this as snk_module_layout_key; without this header a module reports 0 (not checked).
#undef SNK_DETAIL_MODULE_LAYOUT_KEY
#define SNK_DETAIL_MODULE_LAYOUT_KEY (::gs::module_layout_key())
