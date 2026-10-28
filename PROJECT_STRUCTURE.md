<!-- framework-meta: framework=claude-framework class=custom -->

# Project Structure — OpenGL Engine & Editor

```
OpenGL_Engine/
├── CLAUDE.md                       ← Agent instructions (parameterized, < 500 words)
├── MEMORY_RULES.md                 ← Cross-session memory rules (parameterized)
├── PROJECT_STRUCTURE.md            ← This file (custom)
├── .claude-framework               ← Framework manifest (universal, do not edit)
├── .claudesignore                  ← AI context exclusions (custom)
├── .gitignore                      ← Git exclusions
├── .git-blame-ignore-revs          ← Mechanical header-move commits (#109) skipped by blame
├── CMakeLists.txt                  ← Root build configuration
│
├── docs/
│   ├── agent/                      ← Agent documentation (TOON format)
│   │   ├── README.toon             ← Index (custom)
│   │   ├── PROJECT.toon            ← Project overview (custom)
│   │   ├── ARCHITECTURE.toon       ← System design (custom)
│   │   ├── CONSTRAINTS.toon        ← Hard rules (custom)
│   │   ├── CODE_CONVENTIONS.toon   ← Naming and style (custom)
│   │   ├── QUICK_LINKS.toon        ← File navigation (custom)
│   │   ├── learning/               ← Self-learning rules (7 universal files)
│   │   ├── delegation/             ← Subagent delegation rules (6 universal files)
│   │   ├── workflow/               ← Dev process rules (8 universal files)
│   │   └── backlog/                ← Deferred work queue
│   │       ├── BACKLOG-RULES.toon  ← Universal
│   │       ├── SCHEMA.toon         ← Universal
│   │       ├── LIFECYCLE.toon      ← Universal
│   │       ├── OPERATIONS.toon     ← Universal
│   │       ├── SYNC.toon           ← Universal
│   │       ├── INDEX.toon          ← Parameterized (project items)
│   │       └── items/              ← Per-item detail files (runtime)
│   │
│   └── epics/                      ← Project task management
│       ├── INDEX.md                ← Epic registry
│       ├── editor/                 ← Editor feature epics
│       ├── desc-driven/            ← Descriptor architecture epics
│       ├── prefab-first/           ← Prefab system epics
│       ├── render-migration/       ← Render pipeline epics
│       └── infrastructure/         ← Logger, profiler, build epics
│
├── .claude/
│   └── workplan/                   ← Active workflow state (gitignored)
│       └── archive/                ← Completed workplans
│
├── core/                           ← Top-level dir holding BOTH static libs + main.cpp
│   ├── core/                       ← "core" lib: engine foundation
│   │   ├── ecs/                    ← EnTT-based ECS (system_factory, set channel; see docs/roadmap/design/WORLD_MESSAGING.md)
│   │   ├── resource/               ← Resource system (VFS resolvers, adapters, cache)
│   │   ├── desc/                   ← Descriptor system (JSON-based, desc_system)
│   │   ├── config/                 ← Configuration system (include_base/cfg_variables.h: CFG_VAR_DEF_*, base-only; cfg_api.h: read-only config_view)
│   │   ├── application/            ← App lifecycle, module_loader (module states, depends_on order)
│   │   ├── video/                  ← Render driver abstraction (OpenGL impls)
│   │   ├── windows/                ← Platform window service
│   │   ├── input/                  ← Input events
│   │   └── common/                 ← Allocators, ds/, logger/, engine_assert.h
│   │
│   │   Every component below core/ and engine/ keeps its public headers in include/snk/<component>/
│   │   (installed with the SDK, included as <snk/<component>/x.h>); the rest of its directory is internal.
│   │
│   └── engine/                     ← "engine" lib: gameplay + rendering systems
│       ├── scene/                  ← Scene mgmt, descs; scene/level/ (context_manager, context_set, world_context, extractors)
│       ├── render/render_system/   ← Render pipeline (passes/, geom/, shader/, skinning/, texture/)
│       ├── gui/gui_system/         ← ImGui integration
│       ├── image/                  ← Image loading
│       ├── input/                  ← ECS input system
│       └── game_system/            ← Engine bodies of the frame stages (gs_engine_frame_stages); the loop itself is core/core/application (app::loop_service)
│
├── Editor/                         ← Editor application
│   ├── main.cpp                    ← Module bootstrap (Core → Engine → Editor)
│   ├── code/editor_system/         ← Editor sources (see Editor/CLAUDE.md)
│   │   ├── core/                   ← Module lifecycle + frame loop
│   │   ├── controller/             ← editor_system, scene/level/world controllers
│   │   ├── panels/                 ← ImGui panels (extend edt::panel_base)
│   │   ├── gizmo/                  ← Transform gizmo (pure ImGui DrawList)
│   │   ├── import/                 ← Model import/export pipeline
│   │   └── input/                  ← Editor input + file dialog
│   ├── config/                     ← editor.cfg
│   └── res/                        ← Editor resources (levels, objects, templates, shaders, skybox)
│
├── tests/                      ← Boost.Test test suite (auto-discovered)
│
├── lib3dparty/                     ← Third-party submodules
│   ├── assimp/
│   ├── glfw/
│   ├── imgui/
│   └── yaml/
│
├── claude-framework/               ← Local copy of framework (legacy, see Mirage/claude-framework/)
│
└── build/                          ← CMake build output (gitignored)
    ├── Editor/                     ← Editor build artifacts
    └── unit_tests.exe             ← Test executable (Windows / Ninja)
```

## File Classes

| Class | Examples | Patch behavior |
|-------|----------|----------------|
| **universal** | `docs/agent/learning/*.toon`, `docs/agent/delegation/*.toon`, `docs/agent/workflow/*.toon`, `docs/agent/backlog/{BACKLOG-RULES,SCHEMA,LIFECYCLE,OPERATIONS,SYNC}.toon`, `.claude-framework` | Overwritten on patch (after approval) |
| **parameterized** | `CLAUDE.md`, `MEMORY_RULES.md`, `docs/agent/backlog/INDEX.toon` | Three-way merge on patch |
| **custom** | `docs/agent/{README,PROJECT,ARCHITECTURE,CONSTRAINTS,CODE_CONVENTIONS,QUICK_LINKS}.toon`, `PROJECT_STRUCTURE.md`, `.claudesignore` | Never touched by patch |

## Build Targets

```bash
# Configure and build (presets: CMakePresets.json; compiler cache + PCH are enabled by default)
cmake --preset dev-debug
cmake --build build/dev-debug --target editor unit_tests
# (without presets: mkdir build && cd build && cmake .. && make -j$(nproc))

# Run editor
build/dev-debug/Editor/editor --config Editor/config/editor.cfg

# Run all tests
build/dev-debug/unit_tests

# Run specific test suite
build/dev-debug/unit_tests --run_test=DSBitFlagsTests
```

## See Also

- **Agent docs**: [docs/agent/README.toon](docs/agent/README.toon)
- **Architecture**: [docs/agent/ARCHITECTURE.toon](docs/agent/ARCHITECTURE.toon)
- **Constraints**: [docs/agent/CONSTRAINTS.toon](docs/agent/CONSTRAINTS.toon)
- **Epics**: [docs/epics/INDEX.md](docs/epics/INDEX.md)
- **Framework manifest**: [.claude-framework](.claude-framework)
