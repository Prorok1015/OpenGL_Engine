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
- **Memory**: See [Memory Allocators](#memory-allocators) below.
- **ImGui**: Follow `Begin()`/check return/`End()` pattern. Editor logic in `edt` namespace.
- **GLM**: Include only what's needed. Use `scn_glm_json_convert.h` for serialization.

## Memory Allocators

Custom allocators based on `std::pmr::memory_resource`. All live in `core/core/common/mem_*.h`.

### When to use what

| Allocator | Header | Use case |
|-----------|--------|----------|
| `ds::frame_allocator()` | `mem_allocator.h` | **Per-frame temporaries**: render packets, draw call lists, extracted data. Resets every frame automatically. |
| `ds::linear_resource` | `mem_linear_allocator.h` | Custom-scoped bump allocator. Use when you need a reset at a specific point, not per-frame. |
| `ds::pool_resource` | `mem_pool_allocator.h` | Fixed-size objects allocated/freed frequently (components, nodes, handles). O(1) alloc and free. |
| `ds::stack_resource` | `mem_stack_allocator.h` | LIFO allocations with markers. Nested scopes that allocate and then fully unwind. |
| `ds::debug_resource` | `mem_debug_resource.h` | Debug wrapper: guard bytes, leak tracking, double-free detection. Zero overhead in Release. |

### Per-frame allocator (hot path)

```cpp
#include "mem_allocator.h"

// Containers on the hot path use frame_allocator — zero heap allocs per frame:
std::pmr::vector<rnd::render_packet> packets(ds::frame_allocator());
packets.reserve(estimated_count);
// ... fill packets, use them during the frame ...
// Memory is freed automatically at frame start (ds::frame_allocator_reset())
```

**Rules:**
- `ds::frame_allocator_reset()` is called at the start of each frame in the frame loop. All memory from the previous frame is invalidated.
- **Never** store `std::pmr::vector<T>(ds::frame_allocator())` across frames — pointers/iterators become dangling after reset.
- **Always** `reserve()` when the count is known or estimable — avoids grow+copy within the linear buffer.
- `deallocate()` is a no-op on linear allocator. Memory is only reclaimed on `reset()`.
- In Debug builds the frame allocator is wrapped with `debug_resource` — it will assert on leaks at reset and catch buffer overruns.

### Replacing std::vector on hot paths

```cpp
// BEFORE (heap allocation every frame):
std::vector<rnd::draw_call> draw_calls;

// AFTER (bump allocation, no heap):
std::pmr::vector<rnd::draw_call> draw_calls(ds::frame_allocator());
```

For `std::pmr::string`:
```cpp
std::pmr::string name(ds::frame_allocator());
```

### Custom scoped allocators

```cpp
#include "mem_linear_allocator.h"

// 64KB scratch buffer for a specific operation:
ds::linear_resource scratch(64 * 1024);
std::pmr::vector<int> temp(&scratch);
temp.reserve(1000);
// ... use temp ...
// scratch goes out of scope, backing memory freed
```

### Pool allocator

```cpp
#include "mem_pool_allocator.h"

// Pool of 256 blocks, each 128 bytes:
ds::pool_resource pool(128, 256);
void* obj = pool.allocate(128, alignof(MyComponent));
// ... use obj ...
pool.deallocate(obj, 128, alignof(MyComponent));
pool.reset(); // all blocks returned to free-list
```

## Testing

Boost.Test framework. Tests live in `unittests/`. Each file is a `BOOST_AUTO_TEST_SUITE` with `BOOST_AUTO_TEST_CASE` entries. The main entry point `unittests/unit_tests.cpp` defines `BOOST_TEST_MODULE AllTests`. New test files are auto-discovered via `file(GLOB)` in the root CMakeLists.txt — just add a `.cpp` file to `unittests/`.

## Task Management — Epics & User Stories

Все задачи ведутся через систему эпиков в `docs/epics/`. Язык документации — **русский** (описания, мотивация, AC), технические идентификаторы — латиница.

### Иерархия

```
INDEX.md                          ← реестр всех эпиков, граф зависимостей
  └── <theme>/EPIC-NN.md          ← эпик: цель, дизайн, задачи, критерии
        └── US-NN-X (внутри эпика) ← user story: конкретная единица работы
```

| Юнит | Что это | Где живёт | Что содержит |
|------|---------|-----------|-------------|
| **INDEX.md** | Единый реестр | `docs/epics/INDEX.md` | Таблица тем, таблица всех эпиков со статусами, граф зависимостей |
| **EPIC-NN.md** | Эпик — крупная фича или рефакторинг (5–15 user stories) | `docs/epics/<theme>/EPIC-NN.md` | Метаданные, мотивация, архитектура, список US, критерии завершения, риски |
| **US-NN-X** | User Story — минимальная единица работы (1 коммит – 1 день) | Секция внутри EPIC-NN.md | Файлы, описание, acceptance criteria (чеклист) |

### Темы (Theme)

Тема — тематическая папка в `docs/epics/`. Каждый эпик принадлежит ровно одной теме.

| Папка | Описание |
|-------|---------|
| `editor/` | UI-панели, инструменты, pipeline ассетов |
| `desc-driven/` | Переход к world_desc как источнику правды |
| `prefab-first/` | Префабная иерархия, type-safe редактор |
| `render-migration/` | Data-driven рендер конвейер |
| `infrastructure/` | Логгер, профайлер, сборка, ассерты |

Новую тему можно добавить, создав папку и строку в таблице Themes в INDEX.md.

### Статусы

Три статуса, одинаковые для эпиков и user stories:

| Статус | Значение |
|--------|---------|
| `planned` | Описан, но работа не начата |
| `in_progress` | Активная работа (для эпика — хотя бы одна US в работе) |
| `done` | Завершён, все критерии выполнены |

### Нумерация

- **EPIC-NN** — глобальный автоинкремент по всему проекту (не по теме). Следующий ID = максимальный существующий + 1.
- **US-NN-X** — `NN` = номер эпика, `X` = порядковый номер внутри эпика. Пример: `US-33-4` = четвёртая story в EPIC-33.

### Формат INDEX.md

```markdown
# Epics Index

## Themes
| Theme | Folder | Описание |
|---|---|---|

## All Epics

### <Theme Name>
| ID | Title | Status |
|---|---|---|
| [EPIC-NN](<theme>/EPIC-NN.md) | Заголовок | planned/in_progress/done |

## Dependency graph
(ASCII-дерево зависимостей между эпиками)
```

### Формат EPIC-NN.md

```markdown
# EPIC-NN: Заголовок

**Status:** planned|in_progress|done
**Theme:** <theme-folder-name>
**Dependencies:** EPIC-X, EPIC-Y (или "none")

---

## Мотивация

Почему нужен этот эпик. Какие проблемы решает. 1–3 абзаца.

## Архитектурное решение

(Опционально, для сложных эпиков)
Диаграммы, псевдокод, новые структуры данных.

## User Stories

### US-NN-1: Заголовок задачи
**Файлы:** `path/to/file.cpp`, `path/to/file.h`
**Зависимости:** US-NN-X (если есть)

Описание: что сделать и почему.

**AC:**
- [ ] Конкретный критерий 1
- [ ] Конкретный критерий 2

### US-NN-2: ...

---

## Порядок выполнения

(ASCII-граф или список — какие US можно параллелить, какие блокируют друг друга)

## Риски

(Опционально)
- Риск → митигация

## Критерии завершения эпика

- [ ] Глобальный критерий 1 (интеграционный, не повторяет AC отдельных US)
- [ ] Глобальный критерий 2
```

### Правила работы

1. **Перед началом работы** — прочитай `docs/epics/INDEX.md`, найди текущий эпик.
2. **Перед началом US** — прочитай весь эпик, пойми контекст и зависимости.
3. **При завершении US** — отметь `- [x]` в AC внутри эпика.
4. **При завершении эпика** — поставь `**Status:** done` в эпике и обнови статус в INDEX.md.
5. **При создании нового эпика** — создай файл в нужной теме, добавь строку в INDEX.md и обнови граф зависимостей.
6. **Не меняй ID** уже существующих эпиков и user stories.
7. **Файловые пути** в US указывай относительно корня репозитория.
8. **AC должны быть проверяемыми** — «работает корректно» плохо; «модель backpack загружается и рендерится с анимацией» хорошо.