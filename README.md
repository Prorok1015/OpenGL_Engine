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

### Getting the sources

```bash
git clone --recursive https://github.com/Prorok1015/OpenGL_Engine.git
# Optional: test 3D models for the editor (~650 MB, separate repo, not fetched by default)
git submodule update --init --checkout projects/sandbox/res/samples
```

`projects/sandbox/res/objects/robot` (small skinned + animated glb) stays in the repo for a quick check; larger test
models live in [OpenGL_Engine_samples](https://github.com/Prorok1015/OpenGL_Engine_samples) (`res://samples/...`).

### Building

```bash
cmake --preset dev-debug        # Ninja, build/dev-debug (see CMakePresets.json for all presets)
cmake --build build/dev-debug
# or without presets: mkdir build && cd build && cmake .. && make -j$(nproc)
```

A compiler cache (ccache; sccache for MSVC) and a shared precompiled header are used automatically
when available: `-DSNK_COMPILER_CACHE=OFF` / `-DSNK_PCH=OFF` turn them off.

Produces two executables:
| Target | Description |
|--------|-------------|
| `Editor/editor` | Scene editor application |
| `unit_tests` | Boost.Test unit test suite |

### Running the Editor

```bash
# With config file
./Editor/editor --config ../Editor/config/editor.cfg

# Open the test project (projects/sandbox: game content and test levels, EPIC-42)
./Editor/editor --config ../Editor/config/editor.cfg --project ../projects/sandbox
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
│   │   ├── config/                 # Configuration: CFG_VAR_* (base-only cfg_variables.h), read-only cfg_api.h
│   │   ├── application/            # App base class, module lifecycle
│   │   ├── windows/                # GLFW window management
│   │   └── video/drivers/          # Graphics API abstraction (RHI)
│   │       ├── interface/          # Abstract driver interface
│   │       └── impls/opengl/       # OpenGL implementation (glad)
│   │
│   └── engine/                     # High-level systems
│       ├── render/render_system/   # Render pipeline, shaders, geometry, textures
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
│   ├── res/                        # Assets (res://)
│   │   ├── shaders/                # GLSL 420/460 shaders + library includes
│   │   ├── objects/                # 3D models (glb, gltf, fbx, obj)
│   │   ├── levels/                 # Level descriptors (.desc)
│   │   └── skybox/                 # Cubemap face textures
│   └── data/                       # Editor-only resources (edt://)
│       ├── icons/                  # Editor branding
│       └── templates/              # Level / world / prefab preview templates
│
├── lib3dparty/                     # Third-party dependencies (git submodules)
│   ├── entt, glfw, glm, imgui, assimp, spdlog, yaml
│   ├── glad/, stb_image/           # Internal libraries
│   └── natvis/                     # Visual Studio debugger visualizers (lib3dparty/natvis)
│
├── tests/                      # Boost.Test suite (180+ test cases)
├── docs/epics/                     # Task tracker (epics & user stories)
└── CMakeLists.txt                  # Root build configuration
```

---

## Architecture

### Module System

The engine is built from modules implementing `app::modules::module_interface`, initialized in topological order of `depends_on()` (priority only breaks ties):

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
| Multi-World | Contexts (one registry each) grouped into sets; cross-context data goes through the set channel, see [WORLD_MESSAGING.md](docs/roadmap/design/WORLD_MESSAGING.md) |
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

Macro-based tunable parameters, defined by the engine and the editor (`<snk/config/cfg_variables.h>`, base-only, not in the SDK; a game module reads them through `gs::module_context::config()`, a read-only `cfg::config_view` from `<snk/config/cfg_api.h>`):

```cpp
CFG_VAR_DEF_INT(my_var, "path.to.var", 42);
CFG_VAR_DEF_FLOAT(speed, "player.speed", 5.0f);

// Access
int value = *my_var;
```

Override via `.cfg` files or CLI: `--set path/to/var 100` (name and value are two arguments).

**Lists.** `CFG_VAR_DEF_STRING_LIST(var, "a/b", "default1", "default2")` defines a list of strings; the variable is `cfg::variable<std::vector<std::string>>`, a module reads it with `cfg::config_view::list("a/b")` (`value("a/b")` gives the JSON array). In yaml a list is a sequence, block (`- x`) or flow (`[x, y]`); on the CLI `--set a/b "[x, y]"` is a flow sequence, `"[]"` an empty list and a value without `[` a list of that one element.

- A list is **replaced as a whole**: a later file (`extends`), a later `--set` or a second `load` never merges with it.
- A scalar where a list is expected (and the other way round) is refused with a warning in the log, the value stays as it was.
- Elements are text (`12` is `"12"`). A null element (`~`, `null`), a nested list or a map are refused, the list stays as it was. Quote what yaml would read otherwise: an element holding `,` (`"[\"a,b\", c]"`; there is no backslash escape, `[a\, b]` is the two elements `a\` and `b`) or starting with `*` (`"*.ini"`).
- A key with no value (`key:` or `key: ~`) is ignored for every kind of variable.

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

Render modes (topology of a draw, `rnd::driver::RENDER_MODE`): `TRIANGLE`, `TRIANGLE_STRIP`, `LINE`, `LINE_STRIP`, `POINT`. Wireframe is not a mode: it is the `polygon` field of the pipeline description (`polygon_mode::LINE`), off by default.

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
- **Asset Browser, Prefab Editor, Console** panels
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

**Sanitizers (Linux, GCC/Clang):** configure with `-DSNK_SANITIZE=address` (ASan + UBSan) or
`-DSNK_SANITIZE=thread` (TSan) in a separate build dir. Only project targets are instrumented
(third-party code is not); the suppression files live in `.github/sanitizers/`.

```bash
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSNK_SANITIZE=address
cmake --build build/asan --target unit_tests
cd build/asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
LSAN_OPTIONS=suppressions=../../.github/sanitizers/lsan.supp ./unit_tests

# TSan: -DSNK_SANITIZE=thread, then
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=../../.github/sanitizers/tsan.supp" ./unit_tests
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
#include <snk/common/eng_profiler.h>

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

All engine libraries use `snk::` namespace aliases (EPIC-41, partially done):

```
snk::common          STATIC      Math, logging, data structures
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

See `CLAUDE.md` and `docs/agent/CODE_CONVENTIONS.toon` for full details.

- **C++ Standard:** C++20, no extensions
- **Namespaces:** 3-4 letter abbreviations (`ds`, `ecs`, `scn`, `gui`, `rnd`, `res`, `inp`, `edt`, `gs`, `cfg`, `wnd`)
- **Naming:** `snake_case` everywhere; private members use `m_` prefix
- **Files:** `prefix_name.hpp` / `prefix_name.cpp` (e.g., `ds_store.hpp`, `scn_level.cpp`)
- **Headers:** `#pragma once`
- **Indentation:** tabs (size 4)
- **Braces:** new line for functions, same line for control flow

---

## License

See [LICENSE](LICENSE).
