# Editor — CLAUDE.md

## Architecture

```
Editor/
├── main.cpp                          # Module bootstrap: Core → Engine → Editor
├── config/editor.cfg                 # resource.path = ../../projects/sandbox/res/ (without --project)
├── data/                             # edt:// (editor.data_path): icons, templates (editor-only)
└── code/editor_system/
    ├── core/                         # Module lifecycle & frame loop
    │   ├── editor_module.cpp/h       # register/initialize/shutdown lifecycle
    │   ├── edt_editor_init.cpp/h     # constructs editor_system; adds the `ui` stage body and the editor_redirect_cameras stage
    │   └── edt_camera_redirect.cpp/h # redirects backbuffer cameras to textures (editor shows them in the viewport)
    │
    ├── controller/                   # Editor controllers (decomposed from editor_system)
    │   ├── edt_editor_system.cpp/h   # composition root: creates the services and editor_context, panels, wires callbacks, registers the built-ins, async orchestration
    │   ├── edt_scene_editor.cpp/h    # entity CRUD, desc tree helpers, serialize_and_push
    │   ├── edt_level_controller.cpp/h # new/load/save/quicksave level, write_to_disk
    │   ├── edt_world_controller.cpp/h # switch/create world
    │   ├── edt_scene_document.h/.cpp # scene_document: the level being edited, dirty flag, commit()
    │   ├── edt_prefab_document.h/.cpp # prefab_document: read-only prefab preview, owns the preview:<tag> set
    │   ├── edt_editor_camera.cpp/h   # camera state POD, save_camera_state / inject_camera
    │   └── edt_editor_layer.cpp/h    # ImGui layer: dockspace (main menu + default layout from the editor_context registries)
    │
    ├── panels/                       # 7 UI panels, all extend panel_base
    │
    ├── edt_component_renderers/      # per-type ImGui renderers for components
    │   ├── edt_component_renderers.cpp/h
    │   └── edt_cr_*.cpp              # skin, animation (camera, light and skybox are drawn from their schemas)
    │
    ├── import/                       # Asset import/export pipeline
    │   ├── edt_model_importer.cpp/h
    │   ├── edt_asset_exporter.cpp/h
    │   ├── edt_asset_export_dialog.cpp/h
    │   ├── edt_async_import_task.h   # import_intermediate: background-thread import result
    │   └── edt_filesystem_assimp_io.cpp/h
    │
    ├── gizmo/                        # Transform gizmo
    │   ├── edt_transform_gizmo.hpp
    │   └── edt_guizmo.hpp
    │
    ├── game_build/                   # Build > Build Game (#434)
    │   ├── edt_build_diagnostics.h   # build_output_parser (MSVC/GCC/Clang/ld/CMake lines -> problems)
    │   ├── edt_build_settings.cpp/h  # build_settings: <root>/.snk/build.json (parse/merge/load, pure)
    │   ├── edt_path_text.h           # utf8_of(path), shared by the game_build files
    │   ├── edt_toolchain.cpp/h       # find_toolchain: cmake + ninja + environment (vswhere/vcvars64 on Windows), pure parsers
    │   └── edt_game_builder.cpp/h    # game_builder: action build.game_module, the build job, its progress window (not modal)
    │
    ├── input/                        # Input binding helpers & file dialog
    │   ├── edt_input_manager.h       # make_input_area / to_framebuffer_size (window units -> framebuffer pixels)
    │   ├── edt_input_binding.h       # bind_item_input: a hovered panel image binds its rect to a context in inp::input_router
    │   └── edt_file_dialog.cpp/h
    │
    └── CMakeLists.txt
```

## Controller Decomposition

`editor_system` was split into 4 classes connected via `scene_document` and callbacks:

| Class | File | Responsibility |
|-------|------|----------------|
| `editor_system` | `controller/edt_editor_system.cpp/h` | Thin coordinator: init, panel wiring, camera, async orchestration |
| `scene_editor` | `controller/edt_scene_editor.cpp/h` | Entity CRUD (create/delete/duplicate/reparent), desc tree helpers, `serialize_and_push`, `sync_ecs_transforms_to_desc` |
| `level_controller` | `controller/edt_level_controller.cpp/h` | Level I/O: new/load/save/quicksave, `write_level_to_disk`, `populate_worlds_from_level`, templates |
| `world_controller` | `controller/edt_world_controller.cpp/h` | Multi-world: `switch_to_world`, `create_world` |

**Documents** (`include/snk/editor/edt_document.h`, `edt_document_manager.h`; `editor_context::documents()`): `document` has `id/kind/title/source/is_dirty/generation/can_save/save/save_as`. `document_manager` holds the one `scene_document` (owned by `editor_system`, the controllers are built on it) and at most one `prefab_document` preview: `scene()`, `active()`, `set_active(id)`, `open_prefab(tag)`, `preview()`, `close(id)` (the scene cannot be closed), `for_each`. It raises `document_opened/closed/activated`.

**`scene_document`** (`controller/edt_scene_document.h`) holds mutable state shared between controllers:
- Level identity: `editor_tag`, `level_tag`, `level_name`; the dirty flag is `is_dirty()` / `mark_clean()`
- Multi-world: `world_descs`, `world_names`, `worlds_systems_list`, `active_world_idx`
- `camera` (editor camera state, survives the world reload)

**`editor_services`** (`include/snk/editor/edt_editor_services.h`): `ctx_manager` (weak_ptr), `level_set()` (the `level` context set), `active_layer()` (the context manager's game layer) and the game services `descs()`, `resources()`, `assembler()`, `sfactory()` — the active layer's, else the `base_*` ones of the `app_data_storage`. Owned by `editor_system` (declared before the controllers, filled in `init()`); the controllers, `prefab_document`, `document_manager`, the override `view_cache`, the state commands and `editor_context` take it by reference. It lives as long as `editor_system`, not as long as a document. **Never keep a game service taken from it**: a game module replaces the game layer, so take them at the moment of use. Store and erase through `writable_resources()` (the base `resource_system`): a module's layer only writes under `memory://modules/<name>/`. The editor's own tools (model importer, asset exporter, icons, templates) stay on the base `desc_system`/`resource_system` of `editor_system`.

**Every edit ends in `scene_document::commit(label, mode)`**: it marks the document dirty, bumps `desc_generation`, pushes the descs into the level (`scene_editor::serialize_and_push`, installed by `scene_editor` through `set_pusher`) and emits `events.document_changed(id, generation)`. A push that finds no level emits nothing. `commit_mode::state_only` skips the push and the announcement (new level, new world: the contexts are already built; nothing was announced before either). `~editor_system` calls `document_manager::shutdown()` (closes the preview, drops the scene) because the context outlives the system. Never set the dirty flag or call `serialize_and_push` from an edit directly. Save / Save As go through `document::save()/save_as()` (handlers set by `editor_system`); the File menu and the exit prompt still go through `file_menu` and `dialog_service`.

**`prefab_document`** (`controller/edt_prefab_document.h`): read-only (`can_save()` false), opened with `document_manager::open_prefab`, owns the set `preview:<tag>` while open.

Controllers communicate via `std::function` callbacks set by `editor_system` during `init()`.

## Panels

| Panel | Header | Purpose |
|-------|--------|---------|
| `scene_hierarchy_panel` | `edt_scene_hierarchy_panel.h` | Prefab node tree + world tabs; callbacks for create/delete/rename/duplicate/reparent |
| `inspector_panel` | `edt_inspector_panel.h` | Component property editor; custom renderer from `inspector_registry`, else the UI generated from the desc schema (`edt_schema_inspector.h`), else generic JSON |
| `viewport_panel` | `edt_viewport_panel.h` | 3D view + transform gizmo + orientation gizmo; fires `on_transform_committed` |
| `game_view_panel` | `edt_game_view_panel.h` | "Game view" (View menu; `editor/show_game_view` opens it at startup): camera picker + image of the picked game camera; the cameras that draw into the picked camera's texture (the worlds of one set share it, `camera_redirector`) get the viewport, all other game cameras are switched off (logic in `edt_game_view_logic.h`, unit-tested) |
| `console_panel` | `edt_console_panel.h` | `add_log(level, msg)`, max 10K entries with 25% trim, counts per level, filter, clear |
| `problems_panel` | `edt_problems_panel.h` | "Problems" (open by default, docked with the console): table of `editor_context::problems()` (severity, file:line relative to the project root, message; notes in the row's tooltip, rows clipped with `ImGuiListClipper`), counts per severity as filters, "Copy" in the row's context menu |
| `blackboard_panel` | `edt_blackboard_panel.h` | "Blackboard" (View menu, closed by default): read-only table of the committed blackboard of a context set (key, value, type, writer) with a set picker and a key filter; the rows and the value text are `edt_blackboard_view.h` (no ImGui, unit-tested) |
| `asset_browser_panel` | `edt_asset_browser_panel.h` | Directory tree + file grid; drag-drop to viewport |
| `dockspace` | `edt_dockspace.h` | ImGui DockSpace + default layout; the menu bar is `draw_main_menu` (`edt_main_menu.h`) |
| `panel_base` | `edt_panel_base.h` | `render()` wraps ImGui::Begin/End; `virtual on_render()` |

All panels are `shared_ptr` members of `editor_system` and are registered in `editor_context::panels()` (`panel_registry`: id, title, `dock_slot`, `open_by_default`; registration order = render order = View menu order). `dockspace` calls `render_all()` each frame. Menu items and shortcuts are `editor_context::actions()` (`action_registry`); `file_menu::register_actions` adds the File items. Every action has `args_schema`/`result_schema` (JSON Schema subset, `snk/command/cmd_json_schema.h`) and runs through `action_registry::invoke(id, json)`; it never reads ImGui input - the menu click or shortcut runs `activate`, which collects arguments via `dialog_service` and then calls `invoke`. `add()` returns a RAII `registration` (keep it as long as the entry should exist). `action_registry` owns a `cmd::command_registry` (engine module `snk::command`: id, schemas, `enabled`, `invoke`) and keeps the editor-only part (label, menu, shortcut, `activate`) in a side table keyed by id; `commands()` is the engine registry. File menu: Save Level (`file.save`, Ctrl+S) overwrites the level, or opens Save As if it has no path; Save Level As... (`file.save_as`, Ctrl+Shift+S) always asks for a path. The window's own close request (X, Alt+F4) is vetoed and goes through `file_menu::exit` (`attach_window_close`), so it asks about unsaved changes like File > Exit. In the editor that registry is the process-wide `cmd::command_registry` that `core_module` puts in `app_data_storage` (engine commands such as `log.read` are registered there; `editor_context` passes it to `action_registry(shared)`); `action_registry()` alone owns a private one (tests).

Services seen by scripts and agents (A2, `edt_service_commands.h`): `job_service` raises `editor_events::job_started/job_progress/job_finished` (progress is polled once per frame by `job_service::pump()` in the `editor_jobs` frame stage) and serves `job.list`, `job.get{id}`, `job.cancel{id}`; every `dialog_service` request has an id, a kind and an answer schema (`list()`), and `dialog.list` / `dialog.answer{id, answer}` resolve it through the same callback a click uses. The engine log is a ring journal (`engine::read_log_journal`, command `log.read`); `engine::add_log_sink` (base-only `<snk/common/logger/engine_log_sinks.h>`) returns an RAII handle (the console panel is one sink, `editor_system::m_log_sink`). `problem_list` (`edt_problem_list.h`, `editor_context::problems()`) holds the problems per source (`set(source, items)` replaces that source's set, `clear(source)`), raises `editor_events::problems_changed(source)` and serves `problems.list{source?, severity?}` (allowed to agents by id); `build_output_parser` (`game_build/edt_build_diagnostics.h`) turns build output lines into `problem`s.

**Building the game module** (#434, `game_build/edt_game_builder.h`): `game_builder` (created by `editor_system::init`, destroyed before the context) registers `build.game_module` (Build > Build Game, Ctrl+B; args `{configuration?: "debuggame"|"release"}`, result `{started, job}`; a second call while a build runs fails with `busy`; `risk = deny_by_default`, so `agent_may_use` keeps it from MCP agents: it runs the project's CMake code). Enabled only with a project open, `<root>/code/CMakePresets.json` present and the shared engine (`SNK_ENGINE_SHARED`). The job (`ds::lane::io`, `job.cancel`, Cancel or Esc in its non-modal progress window) finds the toolchain (`edt_toolchain.h`: Windows = vswhere -> `cmd /u /d /v:off /s /c call <vs>\VC\Auxiliary\Build\vcvars64.bat >nul 2>&1 && set` read raw (`process_options::raw_output`) as UTF-16LE, a path with `& | < > ^ % !` refused, + `VSLANG=1033`, cmake/ninja of VS unless on PATH; Linux = own environment + `LC_ALL=C`, cmake/ninja on PATH; cached per session, failures not), then runs `cmake --preset <preset>` and `cmake --build --preset <preset> [--parallel N] [--verbose]` in `<root>/code` with `SNAKE_SDK_DIR`; every line goes through `build_output_parser` into `problems()` source `build` (cleared at start), ninja's `[n/m]` is the progress. A failure shows a message dialog; a cancel shows nothing. Processes run through an injected runner (`platform::run_process` by default), so the tests fake it. cfg: `build/sdk_dir` (else `<exe>/../lib/cmake/snake_sdk`, else `SNK_BUILD_TREE_SDK_DIR` = `<build>/snake_sdk`), `build/cmake`, `build/vs_path`, `build/vs_min_version` (17.14), `build/configuration` (debuggame). Per build everything comes from a `build_request` (`configuration`, `preset` (empty = configuration), `jobs` (0 = no `--parallel`), `priority` (`low` = `process_options::low_priority`: BELOW_NORMAL / nice 10 for the process group), `cancel_on_restart` (stops the running build and runs once it stopped, latest request only; else `busy`), `verbosity`, `reload_after`) given to `game_builder::start`; the action makes it with `make_manual_build_request` from `options.settings()` (read at every start; the editor loads `<root>/.snk/build.json`: `configuration`, `jobs_mode` auto|manual, `reload_after_build`, `build_before_play` (stored only), `log_verbosity` errors|normal|verbose; unknown keys kept by `merge_build_settings`, bad values warn and default), `options.preset_for` and `options.auto_jobs` (manual jobs, or no `auto_jobs`, = 0: the preset decides). A build stopped for a restart finishes its job as `done`. Configuration: argument > build.json > cfg. Verbosity sets the level of the output tail on success (errors: none, normal: debug, verbose: info; a failure is always warn). After a successful build `options.events->game_module_built` gets `{module, path (absolute build/bin file that exists), configuration, job, reload}` per runtime module; never after a failure, cancel or restart. Loading/hot-reloading the built module is #435.

State queries for agents (A4, `edt_state_commands.h`, registered by `editor_context`): `scene.query`, `node.get`, `component.schema`, `asset.query`, `selection.get`, all read-only. A node is addressed by `ref = {context: world name, path: [16-char hex node ids]}`; `scene.query` pages through the world desc (`page_size` <= 200) with an opaque cursor bound to the document generation (`stale` after an edit). The MCP bridge allows them by command id (`read_commands` in `edt_mcp_bridge_host.cpp`), never by domain.

## Inspector and Asset-Kind Registries

```cpp
// Registration (edt_component_renderers.cpp, called once at init; keep the returned registrations):
registrations.push_back(context.inspectors().add<scn::camera_desc>([](prefab_comp_node& n) -> bool { ... }));

// Lookup (inspector_panel):
auto result = inspectors.invoke(type_name, comp_node);
// result == nullopt  → fall back to generic JSON editor
// result == true     → component was mutated, call on_node_changed
```

**Adding a new component renderer**: implement lambda in `edt_component_renderers/`, register it in `edt_component_renderers.cpp::register_desc_component_renderers()`. A second renderer for the same type is refused with an error.

`editor_context::asset_kinds()` (`asset_kind_registry`) maps a res-relative path suffix to `{open, drop_into_scene, icon}`; the longest matching suffix that has the wanted action wins. Built-in kinds (`prefab` = `.prefab.desc` opens the prefab editor, `desc` = any asset dropped onto the viewport is added to the scene) are in `controller/edt_builtin_asset_kinds.cpp`.

## Desc ↔ ECS Edit Loop

```
User edits in Inspector
  → inspector on_node_changed callback
  → scene_document::commit("Edit node") → scene_editor::serialize_and_push()
      → active_world_desc serialized to JSON
      → desc_system re-parses level desc
      → ecs_assembler re-assembles world registry
  → Viewport reflects changes next frame

Gizmo drag committed
  → on_transform_committed(node_id path, pos, rot, scale)   (path = scn::node_id_component of the entity)
  → scene_editor::resolve_node_path in active_world_desc → { owner, target }
  → target empty: update owner.position / rotation / scale
  → target non-empty (node inside a prefab instance): override_edit::write_transform → override_record on owner
  → scene_document::commit("Move entity")
```

The editor never finds nodes by name (names are display only): hierarchy, viewport and `scene_editor` pass `node_id`s / id paths. Inspector edits of a node inside a prefab instance go through `edt_override_edit.h` (records on the instance node; "Reset to base" drops them).

**ECS → Desc** (called before save): `scene_editor::sync_ecs_transforms_to_desc()` reads `local_transform` of entities with a `node_id_component`, resolves the path, and writes back to the (non-instanced) desc nodes.

## Level JSON Schema

```json
{
  "worlds": [
    {
      "name": "World0",
      "prefab_desc": { /* prefab_node tree */ },
      "systems": ["SystemA", "SystemB"]
    }
  ]
}
```

`level_controller::build_level_json()` is the single source of truth. `level_controller::populate_worlds_from_level()` reconstructs `scene_document::world_descs` on load.

## Multi-World

- `scene_document::world_descs` — vector, indexed by `active_world_idx`
- `scene_document::active_world_desc()` / `active_world_name()` — pointers, null when the level has no worlds (`active_world_idx = -1`, arrays empty; `world_controller::clear_active_world()`). Never index `world_descs[active_world_idx]` directly.
- Hierarchy panel shows a tab per world; switching calls `world_controller::switch_to_world(idx)` which saves editor camera, tears down old registry, rebuilds new one
- `scene_document::worlds_systems_list` — per-world list of system names (parallel to `world_descs`)

**Single source of truth for world order (critical invariant):** the three parallel
`scene_document` arrays (`world_descs` / `world_names` / `worlds_systems_list`) MUST be
built from the same ordered source — the desc-ordered world list. Do NOT build them by
iterating `scn::context_set::for_each`: it walks the contexts by **context id**, which fills
gaps left by removed worlds and so is not desc/insertion order, so with >=2 worlds the parallel arrays desync. Derive names and
per-world systems via `edt::derive_world_arrays` (`controller/edt_level_controller.h`), and
keep `populate_worlds_from_level` as the single build point on load. Covered by
`tests/edt_world_array_sync_tests.cpp`.

**Hot reload:** `scn::context_manager` drops/rebuilds only the `kind==level` contexts of the set `"level"`, so worlds created in the
editor but not yet saved survive. After every reload (world list or systems only) it calls `level_controller::resync_worlds_after_reload()`
(`merge_reloaded_worlds` + `derive_world_arrays`), which keeps the arrays aligned with the level. A reload
whose world desc fails to load is skipped entirely (current level kept, no callback).

## Prefab Node Format (critical)

```
prefab_desc::prefab_node {
  name, position, rotation(Euler deg), scale
  components: unordered_map<string, prefab_comp_node>
  children: vector<prefab_node>
}
prefab_comp_node { type_name, parent_desc, overrides(json::object) }
```

- `children` serialized as a JSON **array** of nodes `{ "__id", "name", "transform", "components", "children" }` (format v2; v1 keyed them by name and is migrated on load)
- `components` key = type_name (e.g. `"mesh_node_desc"`, `"bone_desc"`)
- `rotation` stores Euler degrees: `x=pitch, y=yaw, z=roll`; reconstructed via `yawPitchRoll(y,x,z)`

## Registered Component Descs (as of current state)

| Type string | Desc class | Spawner |
|-------------|-----------|---------|
| `prefab_desc` | `scn::prefab_desc` | `assemble_prefab` |
| `world_desc` | `scn::world_desc` | `assemble_prefab` |
| `camera_desc` | `scn::camera_desc` | `assemble_camera` |
| `directional_light_desc` | `scn::directional_light_desc` | `assemble_directional_light` |
| `skybox_desc` | `scn::skybox_desc` | `assemble_skybox` |
| `mesh_node_desc` | `scn::mesh_node_desc` | `assemble_mesh_node` |
| `skinning_desc` | `scn::skinning_desc` | `assemble_skinning` |
| `bone_desc` | `scn::bone_desc` | `assemble_bone` |
| `animations_desc` | `scn::animations_desc` | `assemble_animations` |
| `keyframes_desc` | `scn::keyframes_desc` | `assemble_keyframes` |
| `object_desc` | `scn::object_desc` | `assemble_object` |
| `anchor_desc` | `scn::scene_anchor_desc` | `assemble_scene_anchor` |

**Registration lives in**: `scn::register_scene_types` (`core/engine/scene/scn_registration_scope.cpp`)
**Adding a new desc type requires**: one `scope.desc<T>()` / `scope.spawner()` line there; unregistration is automatic when the returned `scn::registration_scope` is destroyed.

## Model Import Pipeline

Models (`.glb`, `.obj`, `.fbx`, `.gltf`) are loaded via `scn::model_importer_adapter` (only with `SNK_WITH_ASSIMP=ON`; with OFF there is no adapter, "Import Model" is disabled and loading such a file fails with a "No adapter found" log):
- Outputs `prefab_desc` (not legacy prototype)
- Geometry stored at `memory://path/name.geom.desc` (separate resource)
- Mesh nodes → `mesh_node_desc` components referencing geometry by tag
- Skinning → `skinning_desc` + bone weights registered with `rnd::skinning_manager`
- Bone nodes → `bone_desc` components with offset matrix + global index
- Animations → `animations_desc` on root, `keyframes_desc` on each animated node

Import in editor: File menu → `dialog_service::pick_file` → `editor_system::start_import` → async import → export dialog → auto-add to scene.

## File Dialog

```cpp
file_dialog.add_extension_filter(".glb");
if (file_dialog.show("Title", &is_open)) {
    // file selected, file_dialog.get_selected_path() available
}
// show() returns true exactly once when user confirms
```

Non-blocking ImGui modal — must be called every frame while open.

## Viewport & Gizmo

- Viewport shows the editor camera texture (`__editor_camera_rt`); game cameras (no color target = backbuffer) are redirected to their own textures by the `editor_redirect_cameras` stage (before `extract`)
- `ImTextureID` obtained via the injected `gui::gui_system&` (from `app_data_storage`, no global accessor): `gui.get_backend_interface()->to_imgui(handle)` with a `texture_handle` (`texture_manager::binding(id_of(tag)).handle` or `render_targets::registry::current(tag)`)
- Gizmo modes: Translate / Rotate / Scale (keyboard shortcuts T/R/S)
- Transform committed only on gizmo release (not every frame)
- `edt_transform_gizmo.hpp` — pure ImGui DrawList implementation, no external gizmo lib

## Common Pitfalls

- **EnTT `registry::each()` does not exist** — use `registry.storage<entt::entity>()` to iterate all entities
- **EnTT forward declare** — use `#include <entt/fwd.hpp>`, not `namespace entt { class registry; }`
- **Input routing**: ECS input is routed by `inp::input_router`; a panel binds its hovered image every frame (`bind_item_input`), the context under the cursor gets the full `inp::input_state`, all others a neutral one. Never set `inp::input_focus` by hand
- **Edits go through `scene_document::commit()`**, which calls `serialize_and_push()` - destructive — tears down and rebuilds the entire ECS world; don't hold raw pointers to registry components across it
- **Camera state**: `scene_editor::on_save_camera` runs before the push and `on_inject_camera` after (state in `scene_document::camera`) — otherwise camera resets
- **Memory resources**: `memory://` protocol stores resources at runtime; `res://` resolves from filesystem via `resource.path`
- **`get_field_desc<T>` with inline objects** requires `__type` field present in the JSON object
- **`children` is a JSON array of named nodes (v2)** — iterate via `as_array()`; a v1 object is only valid before `migrate_tree`
- **No C functions** — prefer `std::format_to_n` over `snprintf`/`strncpy`, C++ alternatives over C stdlib
