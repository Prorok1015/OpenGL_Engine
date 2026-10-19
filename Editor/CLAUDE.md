# Editor — CLAUDE.md

## Architecture

```
Editor/
├── main.cpp                          # Module bootstrap: Core → Engine → Editor
├── config/editor.cfg                 # resource.path = ../res/
├── res/                              # icons, levels, objects, shaders, skybox, templates
└── code/editor_system/
    ├── core/                         # Module lifecycle & frame loop
    │   ├── editor_module.cpp/h       # register/initialize/shutdown lifecycle
    │   ├── edt_editor_init.cpp/h     # constructs editor_system; adds the `ui` stage body and the editor_redirect_cameras stage
    │   └── edt_camera_redirect.cpp/h # redirects backbuffer cameras to textures (editor shows them in the viewport)
    │
    ├── controller/                   # Editor controllers (decomposed from editor_system)
    │   ├── edt_editor_system.cpp/h   # thin coordinator: init, panel wiring, async orchestration
    │   ├── edt_scene_editor.cpp/h    # entity CRUD, desc tree helpers, serialize_and_push
    │   ├── edt_level_controller.cpp/h # new/load/save/quicksave level, write_to_disk
    │   ├── edt_world_controller.cpp/h # switch/create world
    │   ├── edt_shared_state.h        # shared mutable state for all controllers
    │   ├── edt_editor_camera.h       # camera state POD
    │   └── edt_editor_layer.cpp/h    # ImGui layer: dockspace (main menu + default layout from the editor_context registries)
    │
    ├── panels/                       # 7 UI panels, all extend panel_base
    │
    ├── edt_component_renderers/      # per-type ImGui renderers for components
    │   ├── edt_component_ui_registry.h
    │   ├── edt_component_renderers.cpp/h
    │   └── edt_cr_*.cpp              # camera, light, skybox, skin
    │
    ├── import/                       # Asset import/export pipeline
    │   ├── edt_model_importer.cpp/h
    │   ├── edt_asset_exporter.cpp/h
    │   ├── edt_asset_export_dialog.cpp/h
    │   ├── edt_async_import_task.h
    │   └── edt_filesystem_assimp_io.cpp/h
    │
    ├── gizmo/                        # Transform gizmo
    │   ├── edt_transform_gizmo.hpp
    │   └── edt_guizmo.hpp
    │
    ├── input/                        # Input binding helpers & file dialog
    │   ├── edt_input_manager.h       # make_input_area / to_framebuffer_size (window units -> framebuffer pixels)
    │   ├── edt_input_binding.h       # bind_item_input: a hovered panel image binds its rect to a context in inp::input_router
    │   └── edt_file_dialog.cpp/h
    │
    └── CMakeLists.txt
```

## Controller Decomposition

`editor_system` was split into 4 classes connected via `shared_state` and callbacks:

| Class | File | Responsibility |
|-------|------|----------------|
| `editor_system` | `controller/edt_editor_system.cpp/h` | Thin coordinator: init, panel wiring, camera, async orchestration |
| `scene_editor` | `controller/edt_scene_editor.cpp/h` | Entity CRUD (create/delete/duplicate/reparent), desc tree helpers, `serialize_and_push`, `sync_ecs_transforms_to_desc` |
| `level_controller` | `controller/edt_level_controller.cpp/h` | Level I/O: new/load/save/quicksave, `write_level_to_disk`, `populate_worlds_from_level`, templates |
| `world_controller` | `controller/edt_world_controller.cpp/h` | Multi-world: `switch_to_world`, `create_world` |

**`shared_state`** (`controller/edt_shared_state.h`) holds mutable state shared between controllers:
- Service references: `ctx_manager`, `assembler`, `sfactory`
- Level identity: `editor_tag`, `level_tag`, `level_name`, `is_dirty`
- Multi-world: `world_descs`, `world_names`, `worlds_systems_list`, `active_world_idx`

Controllers communicate via `std::function` callbacks set by `editor_system` during `init()`.

## Panels

| Panel | Header | Purpose |
|-------|--------|---------|
| `scene_hierarchy_panel` | `edt_scene_hierarchy_panel.h` | Prefab node tree + world tabs; callbacks for create/delete/rename/duplicate/reparent |
| `inspector_panel` | `edt_inspector_panel.h` | Component property editor; uses component_ui_registry, falls back to generic JSON |
| `viewport_panel` | `edt_viewport_panel.h` | 3D view + transform gizmo + orientation gizmo; fires `on_transform_committed` |
| `game_view_panel` | `edt_game_view_panel.h` | "Game view" (View menu; `editor/show_game_view` opens it at startup): camera picker + image of the picked game camera; only that camera gets a viewport (logic in `edt_game_view_logic.h`, unit-tested) |
| `console_panel` | `edt_console_panel.h` | `add_log(level, msg)`, max 10K entries with 25% trim, counts per level, filter, clear |
| `asset_browser_panel` | `edt_asset_browser_panel.h` | Directory tree + file grid; drag-drop to viewport |
| `dockspace` | `edt_dockspace.h` | ImGui DockSpace + default layout; the menu bar is `draw_main_menu` (`edt_main_menu.h`) |
| `panel_base` | `edt_panel_base.h` | `render()` wraps ImGui::Begin/End; `virtual on_render()` |

All panels are `shared_ptr` members of `editor_system` and are registered in `editor_context::panels()` (`panel_registry`: id, title, `dock_slot`, `open_by_default`; registration order = render order = View menu order). `dockspace` calls `render_all()` each frame. Menu items and shortcuts are `editor_context::actions()` (`action_registry`); `file_menu::register_actions` adds the File items. Every action has `args_schema`/`result_schema` (JSON Schema subset, `snk/command/cmd_json_schema.h`) and runs through `action_registry::invoke(id, json)`; it never reads ImGui input - the menu click or shortcut runs `activate`, which collects arguments via `dialog_service` and then calls `invoke`. `add()` returns a RAII `registration` (keep it as long as the entry should exist). `action_registry` owns a `cmd::command_registry` (engine module `snk::command`: id, schemas, `enabled`, `invoke`) and keeps the editor-only part (label, menu, shortcut, `activate`) in a side table keyed by id; `commands()` is the engine registry.

## Component UI Registry

```cpp
// Registration (edt_component_renderers.cpp, called once at init):
registry.register_renderer<scn::camera_desc>([](prefab_comp_node& n) -> bool { ... });

// Lookup (inspector_panel):
auto result = registry.invoke(type_name, comp_node);
// result == nullopt  → fall back to generic JSON editor
// result == true     → component was mutated, call on_node_changed
```

**Adding a new component renderer**: implement lambda in `edt_component_renderers/`, register it in `edt_component_renderers.cpp::register_desc_component_renderers()`. No other file needs changing.

## Desc ↔ ECS Edit Loop

```
User edits in Inspector
  → inspector on_node_changed callback
  → scene_editor::serialize_and_push()
      → active_world_desc serialized to JSON
      → desc_system re-parses level desc
      → ecs_assembler re-assembles world registry
  → Viewport reflects changes next frame

Gizmo drag committed
  → on_transform_committed(node_id path, pos, rot, scale)   (path = scn::node_id_component of the entity)
  → scene_editor::resolve_node_path in active_world_desc → { owner, target }
  → target empty: update owner.position / rotation / scale
  → target non-empty (node inside a prefab instance): override_edit::write_transform → override_record on owner
  → serialize_and_push()
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

`level_controller::build_level_json()` is the single source of truth. `level_controller::populate_worlds_from_level()` reconstructs `shared_state::world_descs` on load.

## Multi-World

- `shared_state::world_descs` — vector, indexed by `active_world_idx`
- `shared_state::active_world_desc()` / `active_world_name()` — pointers, null when the level has no worlds (`active_world_idx = -1`, arrays empty; `world_controller::clear_active_world()`). Never index `world_descs[active_world_idx]` directly.
- Hierarchy panel shows a tab per world; switching calls `world_controller::switch_to_world(idx)` which saves editor camera, tears down old registry, rebuilds new one
- `shared_state::worlds_systems_list` — per-world list of system names (parallel to `world_descs`)

**Single source of truth for world order (critical invariant):** the three parallel
`shared_state` arrays (`world_descs` / `world_names` / `worlds_systems_list`) MUST be
built from the same ordered source — the desc-ordered world list. Do NOT build them by
iterating `scn::level::for_each_world`: it walks the contexts by **context id**, which fills
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

Models (`.glb`, `.obj`, `.fbx`, `.gltf`) are loaded via `scn::model_importer_adapter`:
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
- `ImTextureID` obtained via the injected `gui::gui_system&` (from `app_data_storage`, no global accessor): `gui.get_backend_interface()->get_imgui_texture_from_texture(tex)`
- Gizmo modes: Translate / Rotate / Scale (keyboard shortcuts T/R/S)
- Transform committed only on gizmo release (not every frame)
- `edt_transform_gizmo.hpp` — pure ImGui DrawList implementation, no external gizmo lib

## Common Pitfalls

- **EnTT `registry::each()` does not exist** — use `registry.storage<entt::entity>()` to iterate all entities
- **EnTT forward declare** — use `#include <entt/fwd.hpp>`, not `namespace entt { class registry; }`
- **Input routing**: ECS input is routed by `inp::input_router`; a panel binds its hovered image every frame (`bind_item_input`), the context under the cursor gets the full `inp::input_state`, all others a neutral one. Never set `inp::input_focus` by hand
- **`serialize_and_push()` is destructive** — tears down and rebuilds the entire ECS world; don't hold raw pointers to registry components across it
- **Camera state**: always call `save_editor_camera_state` before `serialize_and_push`, `inject_editor_camera` after — otherwise camera resets
- **Memory resources**: `memory://` protocol stores resources at runtime; `res://` resolves from filesystem via `resource.path`
- **`get_field_desc<T>` with inline objects** requires `__type` field present in the JSON object
- **`children` is a JSON array of named nodes (v2)** — iterate via `as_array()`; a v1 object is only valid before `migrate_tree`
- **No C functions** — prefer `std::format_to_n` over `snprintf`/`strncpy`, C++ alternatives over C stdlib
