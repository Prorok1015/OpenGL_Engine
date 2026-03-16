# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Full build (from repo root)
mkdir build && cd build
cmake ..
make -j$(nproc)

# Build produces two targets:
#   Editor/editor     - the scene editor application
#   unit_tests        - Boost.Test unit test suite

# Run all tests
cd build && ./unit_tests

# Run a single test suite
./unit_tests --run_test=DSBitFlagsTests

# Run a single test case
./unit_tests --run_test=DSBitFlagsTests/DefaultConstructor

# Run editor with config
./Editor/editor --config ../Editor/config/editor.cfg
```

No linter is configured. No CI pipeline exists — only a Nix dev environment in `.idx/dev.nix`.

## Architecture Overview

### Layer Hierarchy (CMake library dependency order)

```
editor (executable)
  └── engine (static lib) — render, scene, gui, input, game systems
        └── core (static lib) — ecs, window, resource, desc, config, application, video
              └── common (INTERFACE) — glm, boost headers, data structures
                    └── Third-party (git submodules in lib3dparty/)
```

Boost 1.84.0 is fetched via CMake FetchContent (json, hana, program_options, asio, test).

### Module System

Application boots via a priority-ordered module chain: **CORE → ENGINE → EDITOR → GAME**.

Each module implements `app::modules::module_interface` with three lifecycle phases:
1. `register_services()` — add types to `app_data_storage`
2. `initialize_services()` — resolve dependencies from storage, perform setup
3. `shutdown_services()` — cleanup in reverse order

Bootstrap sequence in `Editor/main.cpp`:
```
module_loader.add_module(core_module, engine_module, editor_module)
→ register_all_services(storage)
→ initialize_all_services(storage)  // sorted by priority
→ app.run(storage)                  // main loop via app_loop_service_interface
→ shutdown_all_services(storage)    // reverse order
```

### Service Locator (`ds::app_data_storage`)

Type-safe DI container with parent-child scoping. Default policy: `shared_storage_policy` (uses `shared_ptr`).

```cpp
data.construct<MyService>(args...);       // register
auto& svc = data.require<MyService>();    // get (asserts if missing)
auto ptr = data.require_shared<MyService>(); // get (nullptr if missing)
```

### ECS

EnTT-based with engine extensions. Supports up to 5 independent worlds via `world_salt` + `bind_res<WorldID, T>`. Systems inherit `ecs::system_interface` and are auto-registered via `system_factory`. Dependencies are auto-resolved from registry context using `invoker<Candidate>`.

### Resource System

Resources identified by `res::tag`. Pipeline: `VFS Resolver → Adapter (Assimp/stb/text) → Cache (weak or pinned)`. Protocols: `res://` (filesystem), `memory://` (runtime). Supports async loading via `res_handle<T>` with `.then()` callbacks.

### Descriptor System

JSON-based resource definitions with inheritance chain: `level_desc → world_desc → prefab_desc → prefab_node → prefab_comp_node`. Files use `.desc` extension.

### Configuration System

Macro-based tunables: `CFG_VAR_DEF_INT(name, "path.to.var", default)`. Override via `.cfg` files or CLI `--set path.to.var=value`.

## Code Conventions

- **C++20**, no extensions. Tabs (size 4) for indentation.
- **Namespaces**: 3-4 letter abbreviations — `ds`, `ecs`, `scn`, `gui`, `rnd`, `res`, `inp`, `edt`, `gs`, `cfg`, `wnd`.
- **Naming**: `snake_case` everywhere. Private members end with `_`. Enums/macros `UPPER_CASE`.
- **Files**: `prefix_name.hpp`/`.cpp` where prefix matches namespace (e.g., `ds_store.hpp`, `scn_renderer.cpp`).
- **Headers**: `#pragma once`. Include order: module header → stdlib → third-party → project.
- **Braces**: new line for functions, same line for control flow.
- **Components**: POD structs, suffix `_component` if name isn't self-descriptive.
- **Assertions**: Use `ASSERT_MSG`/`ASSERT_FAIL` from `engine_assert.h`. Use `engine_log.h` for logging, not `std::cout`.
- **Containers**: Prefer `ds::fixed_vector<T, N>` in performance-critical code when max size is known.
- **ImGui**: Follow `Begin()`/check return/`End()` pattern. Editor logic in `edt` namespace.
- **GLM**: Include only what's needed. Use `scn_glm_json_convert.h` for serialization.

## Testing

Boost.Test framework. Tests live in `unittests/`. Each file is a `BOOST_AUTO_TEST_SUITE` with `BOOST_AUTO_TEST_CASE` entries. The main entry point `unittests/unit_tests.cpp` defines `BOOST_TEST_MODULE AllTests`. New test files are auto-discovered via `file(GLOB)` in the root CMakeLists.txt — just add a `.cpp` file to `unittests/`.

# Tasks

current tasks are locating at docs/epics folder.
read docs/epics/INDEX.md file to learn current status.
after complete a task or epic you should update their status in task file and INDEX.md file.