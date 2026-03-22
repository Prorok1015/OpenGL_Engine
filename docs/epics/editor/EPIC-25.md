# EPIC-25: Prefab Editor — автономное редактирование prefab-ассетов

**Theme:** editor
**Status:** in_progress
**Depends on:** EPIC-12, EPIC-13, EPIC-14, EPIC-22, EPIC-15 (рекомендуется)

---

## Цель

Дать возможность открывать и редактировать `.prefab.desc` файлы как самостоятельные ассеты — в изолированном мире, отдельно от level editing. Поддержать вложенность prefab'ов (инстансирование), систему override'ов (instance overrides vs prefab defaults), prefab variants и двусторонний Apply/Revert workflow.

> **Мотивация:** сейчас prefab существует только как часть `world_desc`. При drag-and-drop из Asset Browser он разворачивается в `prefab_node` дерево и "растворяется" в мире. Любое изменение общего prefab'а нужно делать вручную в JSON. Prefab Editor даст artist'ам возможность редактировать переиспользуемые ассеты независимо от конкретных сцен.

---

## Архитектура

### Ключевые понятия

| Термин | Описание |
|---|---|
| **prefab source** | Файл `res://objects/robot/robot.prefab.desc` — источник правды |
| **prefab instance** | `prefab_instance { source_prefab, local_overrides }` — размещение prefab в мире |
| **override** | Изменение поля компонента в конкретном инстансе, скрывающее значение из source |
| **prefab variant** | Отдельный `.prefab.desc` файл, наследующий source через `base_prefab` с набором override'ов |
| **prefab context** | Изолированный ECS world + `prefab_desc`, открытый в Prefab Editor |

### Структура данных — расширение `prefab_instance`

Целевой расширенный вариант (engine layer, `scn_prefab_desc.h`):
```cpp
struct prefab_override_entry {
    std::string node_path;         // "Root/Wheel_FL/Brake" — slash path
    std::string component_type;    // "mesh_node_desc"
    boost::json::object fields;    // только переопределённые поля
};

struct prefab_instance {
    res::tag source_prefab;
    glm::vec3 position{0};
    glm::vec3 rotation{0};
    glm::vec3 scale{1};
    std::vector<prefab_override_entry> overrides;
};
```

### Prefab Context — transient world в level

Вместо отдельного изолированного registry, prefab editor **инъектирует transient world** в текущий `scn::level`. Это позволяет render pipeline автоматически обрабатывать мир через стандартные extractors. Transient world не регистрируется в `shared_state` (world_descs, world_names), поэтому невидим для save и hierarchy tabs.

```
scn::level
├── World0 (scene) — в shared_state, сохраняется
└── __prefab_preview (transient) — НЕ в shared_state
      ├── Camera (→ __prefab_camera_rt, per-camera depth/tp)
      ├── Directional Light
      ├── Skybox
      └── [Loaded Prefab]
```

```cpp
edt::prefab_editor_context {
    shared_state&  m_state;
    res::tag       m_source_tag;     // res://... источник
    uint32_t       m_world_id;       // ID мира в level
    bool           m_dirty;
    editor_camera_state m_camera_state;
    std::shared_ptr<inp::ecs_input_manager> m_pp_input; // отдельный input layer
}
```

Открытие prefab создаёт world в level через `level.create_world()`, собирает preview scene из шаблона + загруженный prefab. Закрытие — `level.remove_world()`. Per-camera render targets обеспечивают независимый depth/transparent pipeline.

### Проблема маршрутизации инпута (multi-viewport input)

Архитектура инпута построена на стеке `input_layer` в `inp::input_system`. Слои обрабатываются сверху вниз (LIFO). Каждый слой может захватить событие и заблокировать нижние (`blocks_lower_layers`).

**Текущий стек (до PP):**
```
[TOP]  edt::input_manager     blocks=true   ← блокирует mouse clicks вне main VP rect
[BOT]  inp::ecs_input_manager blocks=false  ← shared, для всех ECS миров
```

**Проблема:** `edt::input_manager` возвращает `true` для mouse click/keyboard events вне main viewport rect (с `invert=true`). Это блокирует `ecs_input_manager` от получения событий, когда мышь над PP панелью (она вне main VP rect). PP камера не получает mouse button state → не может вращаться/перемещаться.

Кроме того, shared `ecs_input_manager` копирует один и тот же `input_state` во ВСЕ миры через `update_input_state_system`. При двух viewport'ах обе камеры двигаются одновременно.

**Решение: per-viewport input layer**

Каждый viewport-panel, которому нужен отдельный ECS-инпут, создаёт собственный `ecs_input_manager`:
- Пушит как input layer ВЫШЕ `edt::input_manager`
- Управляет `input_area` через `ImGui::IsItemHovered()`
- Регистрирует `update_*_input_state` под уникальным именем в `system_factory`
- World template ссылается на своё имя системы

```
[TOP]  pp_ecs_input            blocks=false  ← PP viewport
[MID]  edt::input_manager      blocks=true   ← блокирует клики вне main VP
[BOT]  inp::ecs_input_manager  blocks=false  ← main world
```

**Почему работает:**
- PP layer выше editor → получает события первым, `on_handle_event` всегда `return false`
- Когда мышь над PP: pp_ecs_input захватывает → editor блокирует → shared не получает
- Когда мышь над main VP: pp_ecs_input пропускает (мышь вне PP rect) → editor пропускает (invert, мышь внутри VP) → shared получает
- Каждый мир имеет свой input_state от своего менеджера → камеры не влияют друг на друга

**Масштабирование:** подход обобщается на N viewport'ов. Для двух камер ОДНОГО мира — решается на уровне camera controller (один active, остальные игнорируют).

### Архитектурный слой

| Компонент | Слой | Обоснование |
|---|---|---|
| `prefab_instance::overrides` (расширение) | engine | `scn_prefab_desc.h` уже в engine |
| `prefab_override_applier` (применение override'ов) | engine | нужен и в runtime, и в редакторе |
| `edt::prefab_editor_context` | editor | только editor-time |
| `edt::prefab_editor_panel` | editor | ImGui, `edt` namespace |
| `edt::prefab_variant_creator` | editor | утилита редактора |
| `edt::prefab_instance_inspector` | editor | отображение override'ов в Inspector |

---

## User Stories

### US-25-1 — Открытие prefab в изолированном контексте

**Файлы:**
- `core/engine/scene/level/scn_level.h/.cpp` — добавить `remove_world()`
- Новый `Editor/code/editor_system/controller/edt_prefab_editor_context.h/.cpp`
- Новый `Editor/code/editor_system/panels/edt_prefab_editor_panel.h/.cpp`
- Новый `Editor/res/templates/prefab_preview.desc`
- `Editor/code/editor_system/controller/edt_editor_system.h/.cpp` — `open_prefab_for_edit`, `close_prefab_editor`
- `Editor/code/editor_system/panels/edt_asset_browser_panel.h/.cpp` — double-click callback
- `Editor/code/editor_system/controller/edt_init_helpers.cpp` — wiring

**Поведение:**

Двойной клик на `.prefab.desc` в Asset Browser → `editor_system::open_prefab_for_edit(tag)`:
- Загружает prefab через `res::resource_system`
- Инъектирует transient world `__prefab_preview` в текущий level (НЕ в shared_state)
- Собирает preview scene из `prefab_preview.desc` (свет) + программная камера с per-camera RT
- Загружает prefab в мир через `assemble_prefab`
- Открывает `prefab_editor_panel` — docked ImGui window с viewport
- Заголовок: `"Prefab: robot [*]"` (звёздочка при `is_dirty`)
- Viewport отображает `__prefab_camera_rt` (отдельный от `__editor_camera_rt`)
- Закрытие panel → `level.remove_world("__prefab_preview")`

**AC:**
- [ ] Double-click на `.prefab.desc` открывает prefab editor panel
- [ ] Префаб рендерится в отдельном viewport с освещением
- [ ] Основной viewport (scene) продолжает работать параллельно
- [ ] Закрытие panel удаляет transient world
- [ ] Save level не включает prefab preview world
- [ ] Hierarchy не показывает tab для prefab world

---

### US-25-2 — Редактирование prefab и Apply/Revert

**Файлы:**
- `Editor/code/editor_system/edt_prefab_editor_context.h/.cpp`
- `Editor/code/editor_system/panels/edt_prefab_editor_panel.h/.cpp`

**Workflow:**

```
Пользователь меняет transform / компонент в Prefab Editor
  → prefab_editor_context::serialize_and_push()
      → working_copy сериализуется → re-assemble ECS world контекста
  → Viewport prefab'а обновляется

[Apply]  → working_copy сериализуется в JSON
         → записывается в source_tag (write_to_disk)
         → hot_reload применяет изменения ко всем инстансам в level world
         → is_dirty = false

[Revert] → рабочая копия перезагружается из source_tag
          → ECS world пересобирается
          → is_dirty = false
```

Кнопки Apply / Revert — в тулбаре `prefab_editor_panel` (над viewport).

---

### US-25-3 — Расширение `prefab_instance`: override'ы компонентов

**Файлы:**
- `core/engine/scene/scn_prefab_desc.h` — расширить `prefab_instance`
- Новый `core/engine/scene/scn_prefab_override_applier.h/.cpp` — применение override'ов
- `core/engine/scene/scn_ecs_assembler.cpp` — учёт override'ов при сборке инстанса
- `core/engine/scene/scn_prefab_desc.cpp` — сериализация нового формата

**Алгоритм:** `ecs_assembler` при сборке `prefab_instance` компонента:
1. Загружает source `prefab_desc`
2. Делает глубокую копию root node
3. Вызывает `apply_overrides(copy, instance.overrides)`
4. Собирает ECS из модифицированного дерева

JSON формат инстанса с override'ами:
```json
{
  "__type": "prefab_instance",
  "source_prefab": "res://objects/robot/robot.prefab.desc",
  "position": [0, 0, 5],
  "overrides": [
    {
      "node_path": "Root/Body",
      "component_type": "mesh_node_desc",
      "fields": { "material": "res://materials/red.mat.desc" }
    }
  ]
}
```

---

### US-25-4 — Inspector: редактирование override'ов инстанса

**Файлы:**
- Новый `Editor/code/editor_system/edt_component_renderers/edt_cr_prefab_instance.cpp`
- `Editor/code/editor_system/edt_component_renderers.cpp` — регистрация

**UI в Inspector при выделении `prefab_instance`:**

```
[ Prefab Instance ]
  Source: res://objects/robot/robot.prefab.desc  [Edit Prefab]
  ─────────────────────────────────────────────────────
  Overrides (2):
  ┌ Root/Body — mesh_node_desc
  │   material: res://materials/red.mat.desc    [Revert]
  └ Root/Wheel_FL — mesh_node_desc
      material: res://materials/blue.mat.desc   [Revert]
  ─────────────────────────────────────────────────────
  [Add Override...]   [Revert All]
```

- **[Edit Prefab]** → `editor_system::open_prefab_for_edit(source_tag)` (US-25-1)
- **[Revert]** одного поля — удаляет `prefab_override_entry`, `serialize_and_push`
- **[Add Override...]** — дерево узлов prefab'а для выбора компонента
- Override поля, совпадающие с source значением, подсвечиваются серым

---

### US-25-5 — Prefab Nesting: инстанс prefab внутри prefab

**Файлы:**
- `core/engine/scene/scn_ecs_assembler.cpp` — рекурсивная сборка вложенных инстансов
- `core/engine/game_system/gs_game_init.cpp` — регистрация `prefab_instance` assembler

**Вложение:** `prefab_node` может содержать компонент с `type_name = "prefab_instance"`. При сборке `ecs_assembler` рекурсивно загружает вложенный prefab, применяет override'ы и присоединяет дерево.

Защита от циклов:
```cpp
void assemble_prefab_guarded(
    ecs_assembler& a, entt::registry& reg, entt::entity parent,
    const prefab_desc& prefab, res::tag source,
    ds::fixed_vector<res::tag, 16>& visited_stack); // max глубина = 16
```

Если `source_tag` уже в `visited_stack` → `ASSERT_MSG(false, "Cyclic prefab reference")`.

---

### US-25-6 — Prefab Variants

**Файлы:**
- Новый `Editor/code/editor_system/edt_prefab_variant_creator.h/.cpp`
- `Editor/code/editor_system/panels/edt_prefab_editor_panel.cpp` — меню "Create Variant"
- `core/engine/scene/scn_prefab_desc.h/.cpp` — поддержка `base_prefab` поля

**Концепция:** variant — это отдельный `.prefab.desc` файл с ссылкой на базовый prefab и набором override'ов:

```json
{
  "__type": "prefab_desc",
  "base_prefab": "res://objects/robot/robot.prefab.desc",
  "overrides": [
    {
      "node_path": "Root/Body",
      "component_type": "mesh_node_desc",
      "fields": { "material": "res://materials/damaged.mat.desc" }
    }
  ]
}
```

При загрузке `desc_system` разрешает `base_prefab`, загружает базовый `prefab_desc`, применяет override'ы, возвращает итоговое дерево.

**UI:** кнопка "Create Variant..." в тулбаре → диалог выбора имени и папки → записывает файл → открывает в новой вкладке.

---

### US-25-7 — Async load prefab для редактирования

**Файлы:**
- `Editor/code/editor_system/edt_editor_system.h/.cpp`

Открытие большого prefab не должно блокировать UI. Паттерн аналогичен US-24-4 (async level load): `require<prefab_desc>` → poll `is_ready()` → `finish_prefab_open()`.

---

## Новые файлы

```
core/engine/scene/
├── scn_prefab_override_applier.h/.cpp   # US-25-3: применение override'ов

Editor/code/editor_system/
├── edt_prefab_editor_context.h/.cpp     # US-25-1, US-25-2, US-25-7
├── edt_prefab_variant_creator.h/.cpp    # US-25-6
├── panels/
│   └── edt_prefab_editor_panel.h/.cpp   # US-25-1, US-25-2, US-25-6
└── edt_component_renderers/
    └── edt_cr_prefab_instance.cpp       # US-25-4
```

---

## Граф зависимостей

```
US-25-1 (Open Context)
  └── US-25-2 (Apply/Revert)
        └── US-25-7 (Async Load)
US-25-3 (Override Data Model)
  └── US-25-4 (Instance Inspector)
  └── US-25-5 (Nesting)
  └── US-25-6 (Variants)

EPIC-15 (Undo/Redo) — рекомендуется до US-25-2
  Без Undo Apply/Revert — единственный способ отмены.
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Контекст и просмотр** | US-25-1, US-25-7 | Открыть prefab в изолированном мире; async load; viewport |
| **2 — Apply/Revert workflow** | US-25-2 | Редактировать prefab, сохранять на диск, hot-reload в level |
| **3 — Override data model** | US-25-3 | Расширить `prefab_instance`, сериализация, `apply_overrides` |
| **4 — Override Inspector UI** | US-25-4 | Редактировать override'ы инстансов в Inspector |
| **5 — Nesting** | US-25-5 | Вложенные prefab инстансы, защита от циклов |
| **6 — Variants** | US-25-6 | Создание и редактирование variant-файлов |

---

## Риски

| Проблема | Решение |
|---|---|
| Два ECS world существуют одновременно | `prefab_editor_context` держит собственный `entt::registry`, не связанный с level world |
| `serialize_and_push` рвёт ECS-указатели | Внутри prefab context применяется к своему registry; level context не затрагивается |
| Render target для prefab viewport | Отдельный `__color_prefab_rt`, создаётся при открытии, удаляется при закрытии |
| Цикличные ссылки в нестинге | `fixed_vector<res::tag, 16>` visited stack в `assemble_prefab_guarded` |
| Override JSON-walk при сборке | `apply_overrides` мутирует **копию** дерева, не оригинальный source |
| Variant загружается как обычный prefab_desc | `desc_system` прозрачно разворачивает `base_prefab` при десериализации |

---

## Критерии готовности

- [ ] Двойной клик на `.prefab.desc` открывает изолированный Prefab Editor (Фаза 1)
- [ ] Apply сохраняет изменения на диск и hot-reload применяется в level (Фаза 2)
- [ ] Revert восстанавливает рабочую копию из файла (Фаза 2)
- [ ] `prefab_instance` компонент поддерживает `overrides` в JSON (Фаза 3)
- [ ] Inspector показывает список override'ов с возможностью Revert на поле (Фаза 4)
- [ ] Вложенный prefab_instance собирается рекурсивно, цикличные ссылки не крашат движок (Фаза 5)
- [ ] Variant создаётся из существующего prefab'а и редактируется отдельно (Фаза 6)
- [ ] Открытие prefab асинхронно — UI не фризит (US-25-7)
