# Editor — CLAUDE.md

## Architecture

```
Editor/
├── main.cpp                          # Module bootstrap: Core → Engine → Editor
├── config/editor.cfg                 # resource.path = ../res/
├── res/                              # icons, levels, objects, shaders, skybox, templates
└── code/editor_system/
    ├── editor_module.cpp             # register/initialize/shutdown lifecycle
    ├── edt_editor_init.cpp           # constructs editor_system + edt_loop_service
    ├── edt_editor_system.h/cpp       # main controller — see below
    ├── edt_editor_layer.h/cpp        # ImGui layer: dockspace + panel_manager + menus
    ├── edt_frame_loop_service.h/cpp  # app_loop_service_interface impl (delta time)
    ├── edt_component_ui_registry.h   # per-type custom ImGui renderers
    ├── edt_component_renderers/      # camera, light, skybox, skin UI implementations
    └── panels/                       # 7 panels, all extend panel_base
```

## editor_system — Central Controller

**Constructor injects**: `desc_system`, `resource_system`, `render_system`, `gui_system`.
**init() receives**: `app_data_storage` — pulls `level_manager`, `ecs_assembler`, `system_factory`.

Key responsibilities:

| Area | Methods |
|------|---------|
| Level I/O | `new_level`, `load_level`, `save_level`, `quick_save_level` |
| Desc editing | `create_entity`, `delete_entity`, `duplicate_entity` |
| Desc ↔ ECS sync | `serialize_and_push()` (desc→ECS), `sync_ecs_transforms_to_desc()` (ECS→desc) |
| Multi-world | `switch_to_world(idx)`, `create_world(name)`, `m_world_descs` vector |
| Editor camera | `save_editor_camera_state`, `inject_editor_camera` (persists across world switches) |
| Serialization | `build_level_json()`, `write_level_to_disk()`, `load_desc_template()` |

## Panels

| Panel | Header | Purpose |
|-------|--------|---------|
| `scene_hierarchy_panel` | `edt_scene_hierarchy_panel.h` | Prefab node tree + world tabs; callbacks for create/delete/rename/duplicate |
| `inspector_panel` | `edt_inspector_panel.h` | Component property editor; uses component_ui_registry, falls back to generic JSON |
| `viewport_panel` | `edt_viewport_panel.h` | 3D view + transform gizmo + orientation gizmo; fires `on_transform_committed` |
| `console_panel` | `edt_console_panel.h` | `add_log(level, msg)`, counts per level, filter, clear |
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
  → editor_system::serialize_and_push()
      → active_world_desc serialized to JSON
      → desc_system re-parses level desc
      → ecs_assembler re-assembles world registry
  → Viewport reflects changes next frame

Gizmo drag committed
  → on_transform_committed(name, pos, rot, scale)
  → find_node_by_name in active_world_desc
  → update node.position / rotation / scale
  → serialize_and_push()
```

**ECS → Desc** (called before save): `sync_ecs_transforms_to_desc()` reads `local_transform` components and writes back to desc nodes.

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

`build_level_json()` is the single source of truth. `populate_worlds_from_level()` reconstructs `m_world_descs` on load.

## Multi-World

- `m_world_descs` — vector, indexed by `m_active_world_idx`
- `active_world_desc()` — shorthand accessor
- Hierarchy panel shows a tab per world; switching calls `switch_to_world(idx)` which saves editor camera, tears down old registry, rebuilds new one
- `m_worlds_systems_list` — per-world list of system names (parallel to `m_world_descs`)

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

Import in editor: `editor_system::show_file_dialog()` → `m_res.warmup<scn::prefab_desc>(tag)`.

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
- **Input rect**: editor input is gated to viewport rect — mouse outside viewport is not forwarded to ECS
- **`serialize_and_push()` is destructive** — tears down and rebuilds the entire ECS world; don't hold raw pointers to registry components across it
- **Camera state**: always call `save_editor_camera_state` before `serialize_and_push`, `inject_editor_camera` after — otherwise camera resets
- **Memory resources**: `memory://` protocol stores resources at runtime; `res://` resolves from filesystem via `resource.path`
- **`get_field_desc<T>` with inline objects** requires `__type` field present in the JSON object
- **`children` is a JSON object, not array** — iterating via `as_object()`, not `as_array()`
