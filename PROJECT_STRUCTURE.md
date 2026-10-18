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
│   │   ├── ecs/                    ← EnTT-based ECS (system_factory, world_salt, bind_res)
│   │   ├── resource/               ← Resource system (VFS resolvers, adapters, cache)
│   │   ├── desc/                   ← Descriptor system (JSON-based, desc_system)
│   │   ├── config/                 ← Configuration system (cfg_api.h, CFG_VAR_DEF_*)
│   │   ├── application/            ← App lifecycle, module_loader, MODULE_PRIORITY
│   │   ├── video/                  ← Render driver abstraction (OpenGL impls)
│   │   ├── windows/                ← Platform window service
│   │   ├── input/                  ← Input events
│   │   └── common/                 ← Allocators, ds/, logger/, engine_assert.h
│   │
│   └── engine/                     ← "engine" lib: gameplay + rendering systems
│       ├── scene/                  ← Scene mgmt, descs; scene/level/ (scn_level, world, extractors)
│       ├── render/render_system/   ← Render pipeline (passes/, geom/, shader/, skinning/, texture/)
│       ├── gui/gui_system/         ← ImGui integration
│       ├── image/                  ← Image loading
│       ├── input/                  ← ECS input system
│       └── game_system/            ← Frame stages and loop service
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
# Configure and build
mkdir build && cd build && cmake .. && make -j$(nproc)

# Run editor
./Editor/editor --config ../Editor/config/editor.cfg

# Run all tests
./unit_tests

# Run specific test suite
./unit_tests --run_test=DSBitFlagsTests
```

## See Also

- **Agent docs**: [docs/agent/README.toon](docs/agent/README.toon)
- **Architecture**: [docs/agent/ARCHITECTURE.toon](docs/agent/ARCHITECTURE.toon)
- **Constraints**: [docs/agent/CONSTRAINTS.toon](docs/agent/CONSTRAINTS.toon)
- **Epics**: [docs/epics/INDEX.md](docs/epics/INDEX.md)
- **Framework manifest**: [.claude-framework](.claude-framework)
