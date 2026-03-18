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
    │   ├── edt_editor_init.cpp/h     # constructs editor_system + edt_loop_service
    │   └── edt_frame_loop_service.cpp/h  # app_loop_service_interface impl
    │
    ├── controller/                   # Editor controllers (decomposed from editor_system)
    │   ├── edt_editor_system.cpp/h   # thin coordinator: init, panel wiring, async orchestration
    │   ├── edt_scene_editor.cpp/h    # entity CRUD, desc tree helpers, serialize_and_push
    │   ├── edt_level_controller.cpp/h # new/load/save/quicksave level, write_to_disk
    │   ├── edt_world_controller.cpp/h # switch/create world
    │   ├── edt_shared_state.h        # shared mutable state for all controllers
    │   ├── edt_editor_camera.h       # camera state POD
    │   └── edt_editor_layer.cpp/h    # ImGui layer: dockspace + panel_manager + menus
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
    ├── input/                        # Editor input & file dialog
    │   ├── edt_input_manager.cpp/h
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
- Service references: `lvl_manager`, `assembler`, `sfactory`
- Level identity: `editor_tag`, `level_tag`, `level_name`, `is_dirty`
- Multi-world: `world_descs`, `world_names`, `worlds_systems_list`, `active_world_idx`

Controllers communicate via `std::function` callbacks set by `editor_system` during `init()`.

## Panels

| Panel | Header | Purpose |
|-------|--------|---------|
| `scene_hierarchy_panel` | `edt_scene_hierarchy_panel.h` | Prefab node tree + world tabs; callbacks for create/delete/rename/duplicate/reparent |
| `inspector_panel` | `edt_inspector_panel.h` | Component property editor; uses component_ui_registry, falls back to generic JSON |
| `viewport_panel` | `edt_viewport_panel.h` | 3D view + transform gizmo + orientation gizmo; fires `on_transform_committed` |
| `console_panel` | `edt_console_panel.h` | `add_log(level, msg)`, max 10K entries with 25% trim, counts per level, filter, clear |
| `asset_browser_panel` | `edt_asset_browser_panel.h` | Directory tree + file grid; drag-drop to viewport |
| `dockspace` | `edt_dockspace.h` | ImGui DockSpace + menu bar (File/Edit/View/Help) |
| `panel_base` | `edt_panel_base.h` | `render()` wraps ImGui::Begin/End; `virtual on_render()` |

All panels are `shared_ptr` members of `editor_system`. `panel_manager` calls `render_all()` each frame.

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
  → on_transform_committed(name, pos, rot, scale)
  → scene_editor::find_node_by_name in active_world_desc
  → update node.position / rotation / scale
  → serialize_and_push()
```

**ECS → Desc** (called before save): `scene_editor::sync_ecs_transforms_to_desc()` reads `local_transform` components and writes back to desc nodes.

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
- `shared_state::active_world_desc()` — shorthand accessor
- Hierarchy panel shows a tab per world; switching calls `world_controller::switch_to_world(idx)` which saves editor camera, tears down old registry, rebuilds new one
- `shared_state::worlds_systems_list` — per-world list of system names (parallel to `world_descs`)

## Prefab Node Format (critical)

```
prefab_desc::prefab_node {
  name, position, rotation(Euler deg), scale
  components: unordered_map<string, prefab_comp_node>
  children: vector<prefab_node>
}
prefab_comp_node { type_name, parent_desc, overrides(json::object) }
```

- `children` serialized as JSON **object** (key = name), NOT array
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

**Registration lives in**: `core/engine/game_system/gs_game_init.cpp`
**Adding a new desc type requires**: register in `game_init`, add `unregister` in `game_term`.

## Model Import Pipeline

Models (`.glb`, `.obj`, `.fbx`, `.gltf`) are loaded via `scn::model_importer_adapter`:
- Outputs `prefab_desc` (not legacy prototype)
- Geometry stored at `memory://path/name.geom.desc` (separate resource)
- Mesh nodes → `mesh_node_desc` components referencing geometry by tag
- Skinning → `skinning_desc` + bone weights registered with `rnd::skinning_manager`
- Bone nodes → `bone_desc` components with offset matrix + global index
- Animations → `animations_desc` on root, `keyframes_desc` on each animated node

Import in editor: `editor_system::show_file_dialog()` → async import → export dialog → auto-add to scene.

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

- Viewport renders `__color_scene_rt` texture from `scn::renderer_3d`
- `ImTextureID` obtained via `gui::get_system().get_backend_interface()->get_imgui_texture_from_texture(tex)`
- Gizmo modes: Translate / Rotate / Scale (keyboard shortcuts T/R/S)
- Transform committed only on gizmo release (not every frame)
- `edt_transform_gizmo.hpp` — pure ImGui DrawList implementation, no external gizmo lib

## Common Pitfalls

- **EnTT `registry::each()` does not exist** — use `registry.storage<entt::entity>()` to iterate all entities
- **EnTT forward declare** — use `#include <entt/fwd.hpp>`, not `namespace entt { class registry; }`
- **Input rect**: editor input is gated to viewport rect — mouse outside viewport is not forwarded to ECS
- **`serialize_and_push()` is destructive** — tears down and rebuilds the entire ECS world; don't hold raw pointers to registry components across it
- **Camera state**: always call `save_editor_camera_state` before `serialize_and_push`, `inject_editor_camera` after — otherwise camera resets
- **Memory resources**: `memory://` protocol stores resources at runtime; `res://` resolves from filesystem via `resource.path`
- **`get_field_desc<T>` with inline objects** requires `__type` field present in the JSON object
- **`children` is a JSON object, not array** — iterating via `as_object()`, not `as_array()`
- **No C functions** — prefer `std::format_to_n` over `snprintf`/`strncpy`, C++ alternatives over C stdlib
