# EPIC-30: Editor Code Review — баги, производительность, реорганизация

**Theme:** editor / infrastructure
**Status:** planned
**Depends on:** —

---

## Часть 1: Логические ошибки (баги)

### BUG-01 — CRITICAL: Null pointer dereference в UV bounds (model_importer)

**Файл:** `edt_model_importer.cpp:316-319`

Цикл обращается к `mesh->mTextureCoords[0]` **без проверки** `HasTextureCoords(0)`. Если модель не имеет UV-координат — crash.

```cpp
// Строка 311-327: face loop — UV bounds
for (unsigned int j = 0; j < face.mNumIndices; j += 3) {
    auto uv1 = (glm::vec2)convert_to_glm(mesh->mTextureCoords[0][face.mIndices[j]]);
    // ^^^ mTextureCoords[0] может быть nullptr!
```

**Исправление:** обернуть блок UV bounds в `if (mesh->HasTextureCoords(0))`.

---

### BUG-02 — CRITICAL: Out-of-bounds при face.mNumIndices % 3 != 0

**Файл:** `edt_model_importer.cpp:316-319`

Цикл `j += 3` обращается к `face.mIndices[j+1]` и `[j+2]`, но не проверяет что `j+2 < face.mNumIndices`. Если face имеет 5 вершин — OOB access.

**Исправление:** добавить `if (j + 2 >= face.mNumIndices) break;` или `j + 2 < face.mNumIndices` в условие цикла.

---

### BUG-03 — MAJOR: Samplers array с дырками (model_importer)

**Файл:** `edt_model_importer.cpp:231-241`

```cpp
if (spec exists) { samplers.resize(2); samplers[1] = spec; }
if (norm exists) { samplers.resize(3); samplers[2] = norm; }
```

Если есть normal map, но нет specular — `samplers[1]` остаётся `json::value()` (null). Шейдер получит невалидный sampler в слоте 1.

**Исправление:** использовать `push_back` + фиксированное присвоение слотов, или заполнять пустые слоты fallback-текстурой.

---

### BUG-04 — MAJOR: async capture raw pointer (export_dialog)

**Файл:** `edt_asset_export_dialog.cpp:150-155`

```cpp
auto* exporter_ptr = &m_exporter;  // ← reference member
m_export_future = std::async(std::launch::async,
    [exporter_ptr, ...] { return exporter_ptr->export_to_project(...); });
```

`m_exporter` — ссылка на `asset_exporter`, время жизни которой не контролируется. Если `export_dialog` уничтожается до завершения async task — use-after-free.

**Исправление:** `m_export_future.wait()` в деструкторе, или передавать `shared_ptr` вместо raw pointer.

---

### BUG-05 — MAJOR: Unsafe as_object() без type check (model_importer)

**Файл:** `edt_model_importer.cpp:620-623`

```cpp
if (prefab_root.contains("components"))
    prefab_root["components"].as_object()["animations_desc"] = anim_comp;
```

Нет проверки что `"components"` — это объект. Если это массив или строка — UB / exception.

**Исправление:** `if (prefab_root.contains("components") && prefab_root["components"].is_object())`.

---

### BUG-06 — MINOR: resource watcher без cleanup в деструкторе (editor_system)

**Файл:** `edt_editor_system.cpp:426-428`

```cpp
m_res.watch(m_editor_tag, this, [this](const res::tag&) {
    on_editor_world_reloaded();
});
```

Callback захватывает `this`. Деструктор `editor_system` пустой — если resource watcher сработает после уничтожения объекта, будет dangling reference.

**Исправление:** добавить `m_res.unwatch(m_editor_tag, this)` в деструктор.

---

### BUG-07 — MINOR: Unbounded console log (console_panel)

**Файл:** `panels/edt_console_panel.cpp:12-14`

`m_logs.push_back(...)` без лимита. Длительная сессия → утечка памяти.

**Исправление:** circular buffer или `if (m_logs.size() > MAX_LOGS) m_logs.erase(m_logs.begin());`

---

## Часть 2: Проблемы производительности

### PERF-01 — sync_ecs_transforms_to_desc: O(n²)

**Файл:** `edt_editor_system.cpp:1085-1098`

Для каждого ECS entity с `name_component` вызывается `find_node_by_name()`, который рекурсивно обходит всё дерево. При 500 entities и 500 nodes — ~250,000 строковых сравнений.

**Вызывается:** перед каждым save.

**Исправление:** построить `unordered_map<string, prefab_node*>` один раз перед циклом.

---

### PERF-02 — make_unique_key: O(n²) при массовом создании

**Файл:** `edt_editor_system.cpp:904-918`

`has_name()` лямбда линейно обходит children на каждую проверку кандидата. При создании 100 entities с одинаковым базовым именем: O(100 * n).

**Исправление:** `unordered_set<string>` из имён перед циклом.

---

### PERF-03 — Аллокации строк в draw_node() на каждый кадр

**Файл:** `panels/edt_scene_hierarchy_panel.cpp:242-244, 274`

```cpp
const std::string prefix  = get_type_prefix(node);     // копия
const std::string label   = prefix + node.name;         // аллокация
const std::string uid     = "##" + node.name;           // аллокация
const std::string node_id = label + uid;                // аллокация
```

4 аллокации на каждый узел, 60 fps × 100 узлов = 24,000 аллокаций/сек.

**Исправление:** использовать `char buf[512]` со `snprintf` или `std::string` с `.reserve()`.

---

### PERF-04 — node_matches_filter: повторный обход поддеревьев

**Файл:** `panels/edt_scene_hierarchy_panel.cpp:67-75`

При активном фильтре каждый узел рекурсивно проверяет всех потомков. Потом при рисовании дочерние узлы снова проверяют своих потомков. Экспоненциальная сложность для глубоких деревьев.

**Исправление:** закешировать результат фильтрации один раз перед отрисовкой (пометить `bool matches` на каждом узле).

---

### PERF-05 — toLower каждого имени в filter на каждый кадр

**Файл:** `panels/edt_scene_hierarchy_panel.cpp:69-70`

```cpp
std::string lower_name = node.name;  // copy
std::transform(..., ::tolower);      // transform
```

Происходит для каждого узла рекурсивно при активном фильтре.

**Исправление:** кешировать lowercase имена или хранить filter hash.

---

### PERF-06 — Assertions внутри vertex loop (model_importer)

**Файл:** `edt_model_importer.cpp:284-308`

`ASSERT_MSG(0 == geometry.layout.get_element_offset("position"), ...)` вызывается на **каждой вершине**. Для модели с 100K вершин — 100K string lookup + assertion.

**Исправление:** проверять layout один раз перед циклом.

---

## Часть 3: Код-запахи и архитектурные проблемы

### SMELL-01 — God Object: editor_system (1098 строк)

`editor_system` совмещает:
- Level I/O (new/load/save/quicksave)
- Desc editing (create/delete/duplicate/reparent entity)
- Multi-world management (switch/create world)
- Camera persistence (save/inject state)
- Serialization (build JSON, write to disk, populate from level)
- Panel wiring (callbacks, selection sync)
- Async import/export orchestration

**7 зон ответственности** в одном классе.

### SMELL-02 — edt_editor_system.h: 27 includes

Каждое изменение в любом include (панель, система, диалог) перекомпилирует весь редактор. Forward declarations не используются.

### SMELL-03 — init() метод: 192 строки

Инициализирует панели, регистрирует колбэки, настраивает меню, логирование — всё в одном методе.

### SMELL-04 — edt_transform_gizmo.hpp: 376 строк в header

Вся логика рендеринга и хит-тестинга гизмо в `.hpp` — включается в каждый файл, который его использует.

---

## Часть 4: Целевая структура файлов

### Текущая структура (51 файл, ~6800 строк)

```
Editor/code/editor_system/
├── 19 файлов в корне (всё свалено вместе)
├── edt_component_renderers/ (5 файлов)
└── panels/ (16 файлов)
```

### Целевая структура

```
Editor/code/editor_system/
│
├── core/                              ← модуль, init, frame loop
│   ├── editor_module.cpp/h
│   ├── edt_editor_init.cpp/h
│   └── edt_frame_loop_service.cpp/h
│
├── controller/                        ← главный контроллер (разбитый)
│   ├── edt_editor_system.cpp/h        ← тонкий координатор (~200 строк)
│   ├── edt_level_controller.cpp/h     ← new/load/save/quicksave level
│   ├── edt_scene_editor.cpp/h         ← create/delete/duplicate/reparent + find_node helpers
│   ├── edt_world_controller.cpp/h     ← switch/create world, world names
│   └── edt_editor_camera.h            ← camera state POD
│
├── panels/                            ← UI панели (без изменений)
│   ├── edt_panel_base.cpp/h
│   ├── edt_panel_manager.cpp/h
│   ├── edt_dockspace.cpp/h
│   ├── edt_scene_hierarchy_panel.cpp/h
│   ├── edt_inspector_panel.cpp/h
│   ├── edt_viewport_panel.cpp/h
│   ├── edt_console_panel.cpp/h
│   └── edt_asset_browser_panel.cpp/h
│
├── component_renderers/               ← UI рендереры для компонентов
│   ├── edt_component_ui_registry.h
│   ├── edt_component_renderers.cpp/h
│   ├── edt_cr_internal.h
│   ├── edt_cr_camera.cpp
│   ├── edt_cr_light.cpp
│   ├── edt_cr_skybox.cpp
│   └── edt_cr_skin.cpp
│
├── import/                            ← pipeline импорта/экспорта ассетов
│   ├── edt_model_importer.cpp/h
│   ├── edt_asset_exporter.cpp/h
│   ├── edt_asset_export_dialog.cpp/h
│   ├── edt_async_import_task.h
│   └── edt_filesystem_assimp_io.cpp/h
│
├── gizmo/                             ← гизмо трансформации
│   ├── edt_transform_gizmo.h          ← интерфейс
│   ├── edt_transform_gizmo.cpp        ← реализация (вынести из .hpp)
│   └── edt_guizmo.hpp                 ← legacy (удалить если не используется)
│
├── input/                             ← editor input
│   ├── edt_input_manager.cpp/h
│   └── edt_file_dialog.cpp/h
│
└── CMakeLists.txt
```

---

## User Stories

### US-30-1 — Исправить критические баги

**Файлы:**
- `edt_model_importer.cpp` — BUG-01, BUG-02, BUG-03, BUG-05
- `edt_asset_export_dialog.cpp` — BUG-04
- `edt_editor_system.cpp` — BUG-06

**Критерии:** все CRITICAL и MAJOR баги исправлены, тесты проходят.

---

### US-30-2 — Разбить editor_system на контроллеры

Извлечь из `edt_editor_system.cpp` (1098 строк):

| Новый класс | Ответственность | Строки |
|---|---|---|
| `edt_level_controller` | new/load/save/quicksave, build_level_json, write_to_disk, populate_worlds | ~300 |
| `edt_scene_editor` | create/delete/duplicate/reparent entity, find_node/remove_node helpers, serialize_and_push, sync_transforms | ~250 |
| `edt_world_controller` | switch_to_world, create_world, m_world_descs/names | ~100 |
| `edt_editor_system` | тонкий координатор: init, panel wiring, async orchestration | ~200 |

**Критерии:** каждый файл < 350 строк, компилируется, тесты проходят.

---

### US-30-3 — Реорганизация файлов по папкам

Переместить файлы в целевую структуру (см. Часть 4). Обновить `CMakeLists.txt`.

**Критерии:** структура соответствует плану, сборка работает.

---

### US-30-4 — Оптимизации производительности

- PERF-01: `unordered_map` в `sync_ecs_transforms_to_desc`
- PERF-03: stack buffer вместо `std::string` в `draw_node`
- PERF-04: кеш фильтрации в hierarchy panel
- PERF-06: assertions вне цикла в model_importer

**Критерии:** профиль draw_node ~0 аллокаций на кадр для 100 узлов.

---

### US-30-5 — Уменьшить include dependencies в edt_editor_system.h

- Forward declarations для всех панелей и систем
- Тяжёлые includes только в .cpp
- `edt_transform_gizmo.hpp` → `.h` + `.cpp`

**Критерии:** `edt_editor_system.h` имеет < 10 includes; панельные .h не pull-ят boost/json.

---

### US-30-6 — Лимит консольных логов + деструктор cleanup

- BUG-07: circular buffer или лимит 10,000 записей в console_panel
- BUG-06: unwatch в деструкторе editor_system

---

## Граф зависимостей

```
US-30-1 (Исправить баги)  ← первый, standalone
  └── US-30-6 (Лимит логов + cleanup)

US-30-2 (Разбить editor_system)
  └── US-30-3 (Реорганизация папок)
        └── US-30-5 (Уменьшить includes)

US-30-4 (Оптимизации)  ← standalone, можно параллельно
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Баги** | US-30-1, US-30-6 | Все критические ошибки исправлены |
| **2 — Декомпозиция** | US-30-2 | editor_system разбит на 4 класса |
| **3 — Файловая структура** | US-30-3, US-30-5 | 7 подпапок, < 10 includes в header |
| **4 — Производительность** | US-30-4 | 0 аллокаций в hot path draw_node |

---

## Риски

| Проблема | Решение |
|---|---|
| Большой diff при перемещении файлов | Один коммит = одна папка; git отследит rename |
| Циклические зависимости при декомпозиции | Чётко определить направление: controller → panels (не наоборот) |
| Регрессии после декомпозиции | Тестировать после каждого US; вручную проверить load/save/create/delete |

---

## Критерии готовности

- [x] Все CRITICAL/MAJOR баги исправлены (US-30-1)
- [ ] `editor_system.cpp` < 300 строк
- [ ] Файлы организованы по 7 подпапкам
- [ ] `edt_editor_system.h` имеет < 10 includes
- [x] `draw_node` не аллоцирует строки на каждый кадр (US-30-4)
- [x] Console имеет лимит 10,000 записей (US-30-6)
- [x] Деструктор editor_system корректно чистит watchers (US-30-6)
- [x] `sync_ecs_transforms_to_desc` использует O(1) lookup (US-30-4)
- [x] Assertions в model_importer вынесены из vertex loop (US-30-4)
- [ ] Сборка работает, тесты проходят, сцены загружаются
