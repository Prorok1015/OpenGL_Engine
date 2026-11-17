# История ABI игровых модулей

Записи, которые до #1056 копились в комментариях кода: над `SNK_MODULE_API_VERSION` (`core/core/application/include/snk/application/app_module_entry.h`) и у `gs::detail::layout_revision` (`core/engine/game_system/include/snk/game/gs_module_layout.h`). Тексты перенесены как были, на английском.

Новые записи сюда не дописываются: каждое изменение кладёт свой файл в [`changes/`](changes/README.md). Когда окно 4.0 закроется (или к релизу), записи из `changes/` переносятся сюда под заголовком версии, а файлы удаляются.

## SNK_MODULE_API_VERSION (от 2.0 до окна 4.0)

```text
2.0 (#595): resource control blocks and res_dep, which inline engine code compiled into a module uses, changed layout.
3.0 (#603): the world bus is inline too (late-sender fields added); any change to the layout of ecs::detail::bus_state,
bus_slot_impl or event_writer/event_reader is a major bump. Also changed in 3.0: the layouts of scn::context_set,
context_manager and scene_layer (#603) and the fields #638 appended to world_context, world_desc and context_set.
3.1 (#603): gs::module_context::subscribe; a module that uses it needs an engine of 3.1 or newer.
3.2 (#604): game actions. New ctx types inp::action_state, inp::action_map_ref, inp::input_bound and the engine event
inp::action_event; the bus and every existing type keep their layout, so a module built for 3.0 or 3.1 still loads.
3.3 (#604): inp::fixed_action_state, a new ctx type; nothing else changes layout. action_state::update and
inp::update_actions_system changed signature: they are the engine's, a module reads the ctx types and never calls them.
3.4 (#600): gs::module_context::observe (component observers the hot reload in Play mutes). The mute depth is appended
to the private, heap-held state of scn::event_subscriptions, so no layout a module reads changes; a module that uses
observe needs an engine of 3.4 or newer.
3.5 (#624): inp::action_input, a new ctx type (what of an action reached the layers of a world that declare it).
inp::stack_layer_desc gains `all_actions` at its end (the layer declares every action); nothing that exists moves.
inp::action_input is read by the engine's inp::update_actions alone (no module, template or test DLL reads it); its
element action_reach grew (armed_keys, armed_buttons: the memory of fronts, #624 PR b) before any module could have
been built against it, so the minor stays 3.5. inp::action_injection (#624 PR c, the edges of inp::input_router::inject)
is a ctx type of the same kind: gs::route_input writes it, inp::update_actions reads it, no module does.
inp::world_input and inp::drag_hold are the engine's own (the result and an argument of inp::resolve), which also
gained a parameter, like inp::update_actions_system and action_state::update: a module calls none of them.
3.6 (#614): the optional export snk_module_layout_key, the key of the layouts of the engine types whose size is compiled
into module code (gs::module_layout_key, gs_module_layout.h). The host refuses a module whose key differs from its
own before it creates it; one without the export (3.5 and older) or with key 0 was checked by version only (#922:
a library module without a key is refused).
Changing the size or alignment of a type of that list is a major bump: the key refuses only modules that have it.
#631 (no bump, from 3.6 on): res::tag parses its protocol inline; a module built before #631 parses its own strings the old way until rebuilt.
3.7 (#625): the input stack from game code. gs::module_context::input_stack(set) and gs::input_stack_handle (exported
methods; module_context is made by the engine and gains two members at its end), the ctx type inp::input_stack_control
(a system's list of stack commands, methods exported), inp::stack_command and inp::stack_layer_desc::owner already there.
A module built for 3.6 or older never calls them and still loads; one that uses them needs an engine of 3.7 or newer.
gs::input_stack_handle, inp::stack_layer_desc and inp::input_stack_control are in the layout key (#614: the module holds,
builds and allocates them), so the key changes; a module without the key export (3.6 and older) is checked by version only.
3.8 (#551): scn::spawn_prefab, scn::despawn and scn::bind_prefab_spawning, new functions a game module imports, and
the components scn::world_root_component and scn::spawned_root_component; nothing that exists changes layout. A module
that spawns prefabs needs an engine of 3.8 or newer.
3.9 (#697): desc_system::get_field_desc, inline in module code, refuses a reference to user://, cache://, code://, edt:// through
the new export resource_system::refused_block; a module built for 3.9 needs an engine of 3.9. No layout changes.
3.10 (#564): scn::request_level and scn::level_switch, new functions a game module imports, and the enum and struct
they use (scn::level_switch_status, scn::level_switch_state). scn::context_set gains a member at its end; the request
itself is a private ctx type of the engine. Nothing a module reads moves. A module that switches levels needs an
engine of 3.10 or newer.
3.11 (#606): components without a C++ type. scn::register_schema_component, scn::schema_component_registration,
component_catalog::add_schema_component, scn::holds and the blob accessors are new exports; context_manager gains
members at its end (engine-made, not in the layout key). A module that registers one needs an engine of 3.11 or newer.
A module built before 3.11 that walks the catalog must not call entry.contains on such an entry (it asserts in debug
and answers false in release): it learns of them only by being rebuilt, and then asks through scn::holds.
3.12 (#606 PR-2): gs::module_context::register_component and desc::remap_entities are new exports (desc::dyn_blob, which
module code allocates, is unchanged); the module_context
constructor (engine-made) takes a mutable context_manager; world_context gains an apply_commands overload and a member
at its end (engine-made, not in the layout key). An entity field of a runtime blob reads and writes as one of a C++ type:
no entity is null (not 0), a number is read only through a snapshot's entity map. A module that registers a
component through its module_context needs an engine of 3.12 or newer.
3.13 (#563): the engine event type scn::animation_finished (the end of a clip without loop on the world bus). A module
that reads or subscribes to it needs an engine that registers it, 3.13 or newer. Nothing that exists changes layout.
3.14 (#605): the bus journal (ecs::bus_journal, world_bus::enable_journal/journal, ecs::detail::bus_journal_source)
and bus_registry::add<T>(schema, payload_writer) are new; bus_type_impl<T> gains a base, which only modules built for
3.14 have: the journal of a type of an older module holds the place of its events, not their sender or payload.
bus_slot, bus_slot_impl and the layout key are unchanged; bus_state, input_router gain members at their end
(engine-made). inp::action_trace, inp::update_and_trace_actions_system and scn::animation_finished_json are new. A
module that gives its events a payload writer needs an engine of 3.14 or newer.
3.15 (#560): scn::set_visible / scn::is_visible (inline, scn_model.h) hide a mesh by taking its scn::renderable, which the
render data extractor requires of meshes from 3.15 on; an older engine draws the mesh anyway, so a module that hides
meshes needs an engine of 3.15 or newer. mesh_node_desc gains `visible` at its end (engine-made, not in the layout key).
3.16 (#744): inp::action_binding gains the alternatives GAMEPAD_BUTTONS (index 2) and GAMEPAD_AXES (index 3), appended after
the keyboard (0) and the mouse (1): it is still one variant of two bytes, which static_asserts in inp_actions.h hold, so a
module of 3.15 that reads the bindings of an action by its stride and indices still reads right. A module that visits the
bindings must know the new alternatives, which a project meets only when its actions.desc names a gamepad.
scn::input_actions_desc gains `gamepad` at its end (engine-made, not in the layout key). New: inp::gamepad_config, the
gamepad names in inp_actions.h, wnd::gamepad_source, gs::input_actions::rebind and the player's bindings (#736).
3.17 (#789): gs::module_context::mark_document, desc::desc_system::mark_document / register_document / document_types and
desc::schema_registry::set_document are new exports (a type the editor opens as a data document, EPIC-56); nothing that
exists changes layout. A module that marks a document type needs an engine of 3.17 or newer.
3.18 (#751): migration steps with a summary and a context (desc::migration_context, desc::migration_warning,
desc_system::register_migration(type, from, summary, fn), migration_summary, migrate_tree(root, file, warnings),
scn::registration_scope::migration(type, from, summary, fn)) are new exports. A step with a summary is stored in the same
registry as any other, so desc_system keeps its size (scn::scene_layer holds one by value) and a module of 3.16 runs it
through its inline migrate_tree, its warnings going to the log. New overloads: scn::migrate_prefab_v3 and
scn::migrate_camera_v1 with a context, scn::migrate_camera_transform_into_node with a warning callback. A module that
registers a step with a summary needs an engine of 3.18 or newer.
3.19 (#755): res::asset_id with asset_id_from_text / to_text / generate_asset_id and the .resource sidecar
(res::read_asset_meta / write_asset_meta) are new exports; res::write_pretty_json is the project's JSON style, and
desc::write_pretty_json / pretty_json_bytes stay as inline wrappers of it. Nothing that exists changes layout. A module
that calls the new exports needs an engine of 3.19 or newer.
4.0 (#758): opens the window of 4.0 (until the end of stage 3 other breaking changes land in their own PRs under 4.0, the
layout key changing with each). A reference field reads a string or a {guid, path} object through the exports
res::reference_from_json and resource_system::resolve_reference; the cache of resource_system (key_for, warmup,
pin_resource, push_resource_to_*cache, find_cached, resolve_alias) is behind exports, so its layout is the engine's;
res_handle caches its type check (#632); ecs::detail::channel_slot gains pending() and context_set::pending_mail counts
the mail in flight (#668); scene_layer holds desc_system by pointer. A module of 3.x is refused: rebuild it.
Window of 4.0, still 4.0: #802 world_context's accessors are exports (its fields are reordered; layout_revision 1 in
gs_module_layout.h refuses a module built before). #786: every member of desc_system, schema_registry and dynamic_desc
that touches their containers is an export, and the non-template migrate_object/value/typed of 3.16 are gone
(layout_revision 2). #784/#738: the meta func loader_get of a component returns its ecs::sandbox_emplace<T>, the
loop and the remap of entity fields are the engine's; meta_archive and snapshot_get are gone (layout_revision 3).
#790: field_kind::map and std::optional<T> in schemas (layout_revision 4). #785/#826: system_registry is all exports,
the entry and its map node are the engine's; emplace_schema_component takes a const dyn_blob& and the world keeps
the engine's copy in the default resource (layout_revision 5).
#781: the 3.15 overloads of inp::action_map::add (std::vector of the old two-alternative binding) are gone; a module of 3.x is
refused at load, and one built in the window that imports them fails to load on the unresolved symbol.
#782: app::modules::loaded_module_apis, prepared_module::api_version and gs::input_actions::warn_about_old_modules are gone;
gs::input_actions loses m_warned_map and m_warned_revision, so its later members move (layout_revision 6).
#783: inp::action_binding is a struct {kind, code, threshold} (in the layout key), inp::action_map::actions()/size() are exports
(layout_revision 7). Pad bindings take an optional threshold in actions.desc.
#797: desc::override_expectation and desc::override_owner are new exports (no type changes: the layout key stays).
#759: cmd::command_desc gains describe_before_allowed_invoke at its end and joins the layout key (layout_revision 10).
#927: rnd::render_context::draw_fits is added at the end of the vtable, the functions before it keep their places (minor: the layout key stays).
#922: the layout key also lists the engine components (scn::engine_component_types), the ctx types and events a module
reads, the channel types, ds::app_data_storage and the structs passed by value; no type changes (layout_revision 11).
#988 (no bump): desc_system guards its registries with a lock and keeps a migration step or a factory alive while it runs.
Its members are out of line (#786) and scene_layer holds it by pointer (#758), so its new size is not seen by a module;
only the private find_factory/find_migrations change their signatures, no public export changes.
#920: the codec of component_tracker's map gains from_json with a read_context, a new export (no type changes).
#952 (no bump): desc_system::get_field_desc, inline in module code, refuses a reference to the owner itself (or to an alias
of it) through the exports that are there; a module built before keeps such a handle until rebuilt. No layout changes.
res::refused_tag (the tag a refused handle was asked for) is a new export (no type changes: the layout key stays).
#922 PR 2: the key also mixes in the ABI revisions of the interfaces a module calls or fills through a vtable, the
versions of EnTT, Boost and glm and the sizes of their types; no type changes (layout_revision 12). A library module
built with the static C runtime or without a layout key is refused when opened; one compiled into the host is not.
#925: res::resource_system::warn_on_sync_require is a new export; its members are the engine's (no type changes: the layout key stays).
#922 PR 3: scene_layer, input_actions and app_data_storage accessors behind exports (layout_revision 13).
#929: res::resource_system::engine_file and engine::gui::load_ui_font_bytes are new exports (no type changes: the layout key stays).
#1021: scn::geometry_holds and scn::hot_reload_manager::resources are new exports (no type changes).
#993: scn::prefab_instance gains content_of (the file its node is content of) at its end (layout_revision 14); scn::reconcile and scn::expand_instance gain overloads that name that file, the old exports stay.
#1039: scn::spawn_world and a context_set::reload_context overload that names the world's file are new exports (no type changes: the layout key stays).
```

## layout_revision (окно 4.0)

```text
Raised when a layout the list cannot see changes for module code: in the window of 4.0 (#758) a reorder of a
type modules read inline. 1: world_context's accessors out of line (#802). 2: desc_system, schema_registry and
dynamic_desc touch their members only out of line (#786). 3: the sandbox's components move into the world by
engine code through the module's ecs::sandbox_emplace<T> (#784, #738). 4: field_kind::map and std::optional in
schemas (#790): field_kind gains a value and the schema's field table changes for code built before. 5: system_registry
is all exports and the world keeps the engine's copy of a component's default (#785, #826). 6: gs::input_actions loses
its two warning fields, so the members after them move (#782).
7: inp::action_binding is a {kind, code, threshold} struct, not a variant (#783).
8: desc_base gains the tag of its parent and joins the list (#812); the list now sees its size.
9: field_schema gains `nullable` in the padding after its flags, unseen by its size; it and type_schema, which
module code builds too, join the list (#861).
10: cmd::command_desc, which a module builds for gs::game_module::add_command, gains describe_before_allowed_invoke
and joins the list (#759).
11: the engine components (scn::engine_component_types), the ctx types and events a module reads, the channel
types, app_data_storage and the structs passed by value join the list (#922); no type changes.
12: the ABI revisions of the interfaces with a vtable, the versions of EnTT, Boost and glm and the sizes of
entt::registry, boost::json::value and glm::mat4 join the key (#922); no type changes.
13: the accessors of scene_layer and input_actions and the core of app_data_storage are exports; module code no
longer reads their members inline (#922 PR 3).
14: scn::prefab_instance gains content_of at its end (#993).
```
