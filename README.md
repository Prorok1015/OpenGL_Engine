# Snake Engine

Snake Engine (`snk::`) is a modular C++20 game engine built on OpenGL with an integrated scene editor, data-driven resource pipeline, and an ECS architecture powered by EnTT.

> The project started as a simple OpenGL snake game and evolved into a general-purpose engine with a full editor.

```
 ┌────────────────────────────────────────────────────────────┐
 │                     Snake Editor                           │
 │  Toolbar | Hierarchy | Viewport (3D) | Properties Panel   │
 ├────────────────────────────────────────────────────────────┤
 │                      Engine Layer                          │
 │   Scene Graph  |  Renderer  |  GUI (ImGui)  |  Input      │
 ├────────────────────────────────────────────────────────────┤
 │                       Core Layer                           │
 │  ECS | Resources/VFS | Descriptors | Video/RHI | Window   │
 ├────────────────────────────────────────────────────────────┤
 │                   Third-Party Libraries                    │
 │  EnTT | GLFW | GLM | Assimp | ImGui | Boost | spdlog     │
 └────────────────────────────────────────────────────────────┘
```

---

## Getting Started

### Prerequisites

The project supports two development environments:

- **Project IDX** (Nix) — all dependencies pre-configured in `.idx/dev.nix`
- **Local build** — requires CMake 3.16+, a C++20 compiler, Boost, and platform windowing libraries

### Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Produces two executables:
| Target | Description |
|--------|-------------|
| `Editor/editor` | Scene editor application |
| `unit_tests` | Boost.Test unit test suite |

### Running the Editor

```bash
# With config file
./Editor/editor --config ../Editor/config/editor.cfg

# Or override resource path directly
./Editor/editor --set resource/path ../Editor/res
```

---

## Project Structure

```
OpenGL_Engine/
│
├── core/
│   ├── core/                       # Low-level foundation
│   │   ├── common/                 # Data structures, math, logging
│   │   │   ├── ds/                 # event, fixed_vector, rtree, bbox, store, bit_flags
│   │   │   └── logger/             # spdlog-based logging
│   │   ├── ecs/                    # ECS framework (EnTT wrapper)
│   │   ├── resource/               # VFS, async loading, caching, adapters
│   │   ├── desc/                   # Descriptor system (.desc JSON files)
│   │   ├── input/                  # Low-level input events
│   │   ├── config/                 # Macro-based configuration (CFG_VAR_*)
│   │   ├── application/            # App base class, module lifecycle
│   │   ├── windows/                # GLFW window management
│   │   └── video/drivers/          # Graphics API abstraction (RHI)
│   │       ├── interface/          # Abstract driver interface
│   │       └── impls/opengl/       # OpenGL implementation (glad)
│   │
│   └── engine/                     # High-level systems
│       ├── render_system/          # Render pipeline, shaders, geometry, textures
│       ├── scene/                  # Scene graph, cameras, lights, animation
│       ├── gui/                    # ImGui integration (backends + layer system)
│       ├── game_system/            # Game loop, frame timing
│       ├── input/                  # High-level input mapping
│       └── image/                  # Image loading (stb_image)
│
├── Editor/
│   ├── main.cpp                    # Entry point
│   ├── config/editor.cfg           # Editor configuration
│   ├── code/editor_system/         # Editor systems (UI, spawn, input, gizmos)
│   └── res/                        # Assets
│       ├── shaders/                # GLSL 420/460 shaders + library includes
│       ├── objects/                # 3D models (glb, gltf, fbx, obj)
│       ├── levels/                 # Level descriptors (.desc)
│       ├── skybox/                 # Cubemap face textures
│       └── icons/                  # Editor branding
│
├── lib3dparty/                     # Third-party dependencies (git submodules)
│   ├── entt, glfw, glm, imgui, assimp, spdlog, yaml
│   ├── glad/, stb_image/           # Internal libraries
│   └── natvis/                     # Visual Studio debugger visualizers
│
├── unittests/                      # Boost.Test suite (180+ test cases)
├── docs/epics/                     # Task tracker (epics & user stories)
├── guidelines.md                   # Coding standards
└── CMakeLists.txt                  # Root build configuration
```

---

## Architecture

### Module System

The engine is built from modules implementing `app::modules::module_interface`, each with a priority that determines initialization order:

```
CORE  →  ENGINE  →  EDITOR  →  GAME
```

Module lifecycle:
1. `register_services()` — add types to `app_data_storage`
2. `initialize_services()` — fetch dependencies, perform setup
3. `shutdown_services()` — cleanup in reverse order

### Service Locator (`app_data_storage`)

Type-safe dependency injection container with hierarchical parent-child scoping:

```cpp
// Register
data.construct<MyService>(args...);

// Retrieve (asserts if missing)
auto& svc = data.require<MyService>();

// Retrieve (returns nullptr if missing)
auto svc_ptr = data.require_shared<MyService>();
```

### Entity-Component-System (ECS)

Built on [EnTT](https://github.com/skypjack/entt) with engine-specific extensions:

| Feature | Description |
|---------|-------------|
| Components | POD structs (suffix `_component`) |
| Systems | Classes inheriting `ecs::system_interface`, auto-registered via `system_factory` |
| Command Buffer | `sandbox_registry` for deferred spawn/despawn operations |
| Invoker | Automatic dependency resolution from registry context |
| Multi-World | Up to 5 independent registries via `world_salt` + `bind_res<WorldID, T>` |
| Serialization | `sandbox_loader_archive` with entity remapping |

### Resource System

```
 res::tag("objects/robot.glb")
     │
     ▼
 VFS Resolver ──► Adapter (Assimp / stb / text) ──► Cache
                                                      │
                              ┌────────────────────────┤
                              ▼                        ▼
                         Weak cache              Pinned cache
                       (auto-cleanup)           (held in memory)
```

- **Protocols**: `res://` (filesystem), `memory://` (runtime data)
- **Async loading**: `res_handle<T>` with `.then()` callbacks
- **Hot-reload**: file watching via Boost.ASIO
- **Aliasing**: `register_alias()` for tag remapping

### Descriptor System

JSON-based resource definitions with inheritance:

```json
{
  "__type": "prefab_desc",
  "__parent": "res://objects/robot/gen_robot.glb",
  "transform": { "position": [0, 5, 0], "rotation": [0, 90, 0] },
  "children": {
    "child_node": { "__type": "prefab_desc" }
  }
}
```

Hierarchy: `level_desc` → `world_desc` → `prefab_desc` → `prefab_node` → `prefab_comp_node`

### Configuration System

Macro-based tunable parameters:

```cpp
CFG_VAR_DEF_INT(my_var, "path.to.var", 42);
CFG_VAR_DEF_FLOAT(speed, "player.speed", 5.0f);

// Access
int value = *my_var;
```

Override via `.cfg` files or CLI: `--set path.to.var=100`

---

## Rendering Pipeline

Multi-pass forward renderer with OpenGL 4.2+:

```
 1. Sky Pass           ← cubemap sampling, rotation-only view matrix
 2. Z-Prepass          ← early depth rejection
 3. Opaque Pass        ← Phong lighting (directional + point), skinning
 4. Transparent Pass   ← alpha blending
 5. Composition Pass   ← opaque + transparent mixing
```

Pre-allocated buffers: 8M vertices, 800K indices.

**Shader library system** — reusable GLSL includes:
- `phong_model_lib.frag` — Phong shading with directional/point lights, gamma correction
- `skinning_lib.glsl` — skeletal animation via SSBO (bone matrices)
- `pipline_struct_lib.glsl` — shared vertex output struct (UV, TBN, normals)

Render modes: `TRIANGLE`, `LINE_LOOP`, `POINT`, `WIREFRAME`.

---

## Editor

Snake Editor provides a full scene editing environment:

- **Viewport** — 3D scene with camera orbit control and grid overlay
- **Hierarchy** — entity tree with context menus (add/delete/reparent)
- **Properties** — transform, light, camera, animation, material editing
- **Gizmo** — translate/rotate/scale manipulation
- **Import** — model import dialog (glb, gltf, fbx, obj)
- **Animation** — playback controls with keyframe support
- **Skybox** — cubemap-based background
- **Debug tools** — JSON debugger, ECS inspector, R-tree visualizer, texture browser
- **Profiler overlay** — real-time performance stats with smoothing, multi-select rows, copy to clipboard
- **Profiler hotkey** — `F3` dumps frame stats to engine log

---

## Data Structures (`ds::`)

| Structure | Header | Purpose |
|-----------|--------|---------|
| `fixed_vector<T, N>` | `ds_fixed_vector.hpp` | Stack-allocated vector, constexpr, no heap allocation |
| `event<T>` | `ds_event.hpp` | Type-safe event with handle/tag-based subscription |
| `rtree_q<T, BBox>` | `ds_rtree.h` | R-tree spatial index (STR bulk loading) |
| `bbox` | `ds_bbox.hpp` | 2D AABB with intersect/contain/expand |
| `app_data_storage` | `ds_store.hpp` | Policy-based service locator with parent-child hierarchy |
| `bit_flags<Enum>` | `ds_bit_frags.hpp` | Type-safe bitwise flag operations |
| `transform3d` | `eng_transform_3d.hpp` | TRS transform (position + quaternion + scale) |

---

## Dependencies

### Git Submodules (lib3dparty/)

| Library | Purpose |
|---------|---------|
| [EnTT](https://github.com/skypjack/entt) | ECS framework |
| [GLFW](https://github.com/glfw/glfw) | Window management and input |
| [GLM](https://github.com/g-truc/glm) | Linear algebra (vectors, matrices, quaternions) |
| [ImGui](https://github.com/ocornut/imgui) (docking) | Editor UI |
| [Assimp](https://github.com/assimp/assimp) | 3D model import (glb/gltf/fbx/obj) |
| [spdlog](https://github.com/gabime/spdlog) | Logging |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | YAML configuration |

### Internal Libraries

| Library | Purpose |
|---------|---------|
| glad | OpenGL function loader |
| stb_image | Image loading (PNG, JPG, BMP) |

### Fetched via CMake

| Library | Purpose |
|---------|---------|
| Boost (json, asio, test) | JSON parsing, async I/O, unit testing |

---

## Testing

**Framework:** Boost.Test | **180+ test cases across 15+ files**

```bash
cd build && ./unit_tests
```

| Test File | Coverage |
|-----------|----------|
| `ds_fixed_vector_test` | push/pop, overflow, RAII semantics |
| `ds_event_tests` | subscribe/unsubscribe, handle and tag modes |
| `ds_rtree_tests` | build, insert, overlap queries |
| `ds_store_tests` | construct/require, parent-child hierarchy |
| `ds_bit_frags_tests` | bitwise ops, has_flag, clear |
| `ds_polymorphic_cast_tests` | raw/shared_ptr downcasting |
| `transform_3d_tests` | TRS composition, decomposition |
| `transform_system_tests` | hierarchy depth, world matrix propagation |
| `timer_tests` | monotonic clock precision |

---

## Profiling

Built-in profiler with macro API. Enable via CMake:

```bash
cmake --preset x64-Debug -DENGINE_PROFILE_MODE=INTERNAL
```

Usage in code:

```cpp
#include "eng_profiler.h"

void update() {
    PROFILE_FUNCTION();              // auto-named zone
    {
        PROFILE_SCOPE("Physics");    // named zone
    }
}
```

- **ImGui overlay**: View > Profiler — real-time stats with smoothing
- **Log dump**: press `F3` or call `ds::profiler_dump_to_log()`
- **Modes**: `INTERNAL` (ring buffer + overlay), `NONE` (zero overhead, macros compile to nothing)

---

## CMake Targets

All engine libraries use `snk::` namespace aliases (planned via EPIC-41):

```
snk::common          INTERFACE   Math, logging, data structures
snk::application     STATIC      App lifecycle, module system
snk::ecs             STATIC      ECS framework
snk::scene           STATIC      Scene graph, cameras, lights
snk::render          STATIC      Render pipeline, shaders
snk::gui             STATIC      ImGui integration
snk::input_system    STATIC      Input mapping
snk::engine          STATIC      Aggregates all engine systems
```

---

## Code Conventions

See [guidelines.md](guidelines.md) for full details.

- **C++ Standard:** C++20, no extensions
- **Namespaces:** 3-4 letter abbreviations (`ds`, `ecs`, `scn`, `gui`, `rnd`, `res`, `inp`, `edt`, `gs`, `cfg`, `wnd`)
- **Naming:** `snake_case` everywhere; private members end with `_`
- **Files:** `prefix_name.hpp` / `prefix_name.cpp` (e.g., `ds_store.hpp`, `scn_renderer.cpp`)
- **Headers:** `#pragma once`
- **Indentation:** tabs (size 4)
- **Braces:** new line for functions, same line for control flow

---

## License

See [LICENSE](LICENSE).
