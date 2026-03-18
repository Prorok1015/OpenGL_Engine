# EPIC-29: Service Layer Refactoring — удаление game_system, единый паттерн сервисов

**Theme:** infrastructure
**Status:** planned
**Depends on:** —

---

## Цель

1. Удалить `game_system` — перенести регистрацию дескрипторов и spawner'ов в соответствующие модули
2. Переименовать все фактические сервисы с суффиксом `_service` для единообразия
3. Убрать boilerplate из init-функций — выработать единый паттерн добавления нового сервиса
4. Убрать legacy глобальные указатели (`p_res_system`, `p_render_system`, и т.д.)

---

## Проблемы текущей архитектуры

### 1. `game_system` — "мусорная корзина" регистраций

`gs_game_init.cpp` содержит 23 `register_desc` + 13 `register_desc_spawner` для типов, принадлежащих **разным модулям**: `scn::camera_desc` (engine), `rnd::texture_2d_desc` (render), `scn::material_desc` (scene). Все свалены в один файл.

Проблемы:
- Нарушает SRP — game_system знает обо всех типах движка
- При добавлении нового desc нужно редактировать `gs_game_init.cpp` + `gs_game_init.cpp` (unregister)
- Пустой `game_module` (все 3 метода — комментарии) существует без смысла

### 2. Непоследовательное именование сервисов

| Класс | Суффикс | Ожидаемый |
|---|---|---|
| `resource_system` | `_system` | `resource_service` |
| `desc_system` | `_system` | `desc_service` |
| `window_system` | `_system` | `window_service` |
| `render_system` | `_system` | `render_service` |
| `gui_system` | `_system` | `gui_service` |
| `input_system` | `_system` | `input_service` |
| `system_factory` | `_factory` | `ecs_service` или `system_factory_service` |
| `ecs_assembler` | — | `assembler_service` |
| `level_manager` | `_manager` | `level_service` |
| `hot_reload_manager` | `_manager` | `hot_reload_service` |
| `skinning_manager` | `_manager` | `skinning_service` |
| `frame_assembler` | — | `frame_assembler_service` |
| `game_system` | `_system` | **удалить** |
| `editor_system` | `_system` | `editor_service` |

Суффиксы `_system`, `_manager`, `_factory` используются для сервисов без системы.

### 3. Boilerplate при добавлении сервиса

Чтобы добавить новый сервис, нужно:
1. Создать класс `my_thing.h/.cpp`
2. Создать `my_thing_service_init.h/.cpp` с `init()` и `term()` функциями
3. Добавить глобальный указатель `extern MyThing* p_my_thing;`
4. Вызвать `init()` из модуля `register_services()`
5. Вызвать `term()` из модуля `shutdown_services()`
6. Если есть desc — отдельно в `gs_game_init.cpp`

**6 файлов, 4 точки редактирования** для одного сервиса.

### 4. Legacy глобальные указатели

```cpp
extern res::resource_system* p_res_system;    // → res::get_system()
extern rnd::render_system*   p_render_system; // → rnd::get_system()
extern gui::gui_system*      p_gui_system;    // → gui::get_system()
extern gs::game_system*      p_game_system;   // → gs::get_system()
```

Дублируют `app_data_storage`, создают скрытые зависимости, усложняют тестирование.

---

## Целевая архитектура

### Единый паттерн сервиса

Каждый сервис — класс с опциональным `service_interface`:

```cpp
// my_service.h
namespace my {
    class my_service {
    public:
        // Конструктор принимает зависимости напрямую
        my_service(res::resource_service& res, desc::desc_service& desc);

        // Опционально: регистрация desc/spawner внутри сервиса
        void register_descriptors(desc::desc_service& desc, scn::assembler_service& assembler);
        void unregister_descriptors(desc::desc_service& desc, scn::assembler_service& assembler);
    };
}
```

### Регистрация дескрипторов — в своём модуле

Вместо `gs_game_init.cpp` с 36 регистрациями:

```cpp
// engine_module::register_services()
void engine_module::register_services(ds::app_data_storage& data) {
    auto& desc = data.require<desc::desc_service>();

    // Render descriptors — здесь, не в game_system
    desc.register_desc<rnd::texture_2d_desc>("texture_2d_desc");
    desc.register_desc<rnd::texture_cubemap_desc>("texture_cubemap_desc");

    // Scene descriptors
    desc.register_desc<scn::camera_desc>("camera_desc");
    desc.register_desc<scn::material_desc>("material_desc");
    // ...
}
```

### Убрать *_init.h/cpp файлы

Вместо отдельных `*_service_init.cpp` — конструирование прямо в модуле:

```cpp
// engine_module::register_services()
{
    auto& res = data.require<res::resource_service>();
    auto& desc = data.require<desc::desc_service>();
    data.construct<rnd::render_service>(res, desc);
    data.construct<gui::gui_service>(/* deps */);
    // ...
}
```

### Убрать глобальные указатели

Доступ к сервисам — только через `app_data_storage` или инъекцию в конструкторе:

```cpp
// Было:
rnd::get_system().get_texture_manager().find(tag);

// Стало (в конструкторе):
my_service(rnd::render_service& rnd) : m_rnd(rnd) {}
// ...
m_rnd.get_texture_manager().find(tag);
```

---

## User Stories

### US-29-1 — Удалить `game_system`, раскидать регистрации по модулям

**Файлы:**
- Удалить `core/engine/game_system/gs_game_system.h/.cpp`
- Удалить `core/engine/game_system/gs_game_init.h/.cpp`
- Удалить `core/engine/game_system/game_module.h/.cpp`
- `core/engine/engine_module.cpp` — перенести desc + spawner регистрации
- `Editor/code/editor_system/editor_module.cpp` — editor-specific desc

**Перенос регистраций:**

| Дескриптор | Целевой модуль | Обоснование |
|---|---|---|
| `texture_desc`, `texture_2d_desc`, `texture_cubemap_desc` | ENGINE (render) | Рендер-ресурсы |
| `material_desc`, `geometry_desc` | ENGINE (render) | Рендер-ресурсы |
| `camera_desc`, `directional_light_desc`, `skybox_desc` | ENGINE (scene) | Сценарные компоненты |
| `mesh_node_desc`, `object_desc`, `anchor_desc` | ENGINE (scene) | Сценарные компоненты |
| `animations_desc`, `keyframes_desc`, `bone_desc`, `skinning_desc` | ENGINE (scene) | Анимация |
| `prefab_desc`, `world_desc`, `level_desc` | ENGINE (scene) | Уровневая иерархия |

**Критерии:** `game_system` удалён, все desc/spawner регистрируются в `engine_module`, `game_module` удалён.

---

### US-29-2 — Переименовать сервисы с суффиксом `_service`

**Файлы:** все заголовки и .cpp сервисов + все callsite'ы.

**Переименования:**

| Было | Стало |
|---|---|
| `res::resource_system` | `res::resource_service` |
| `desc::desc_system` | `desc::desc_service` |
| `wnd::window_system` | `wnd::window_service` |
| `rnd::render_system` | `rnd::render_service` |
| `gui::gui_system` | `gui::gui_service` |
| `inp::input_system` | `inp::input_service` |
| `ecs::system_factory` | `ecs::system_factory_service` |
| `scn::ecs_assembler` | `scn::assembler_service` |
| `scn::level_manager` | `scn::level_service` |
| `scn::hot_reload_manager` | `scn::hot_reload_service` |
| `rnd::skinning_manager` | `rnd::skinning_service` |
| `rnd::frame_assembler` | `rnd::frame_assembler_service` |
| `edt::editor_system` | `edt::editor_service` |

**Стратегия:** `using` alias на переходный период → удалить alias через 1-2 итерации.

```cpp
// Переходный alias
namespace res {
    class resource_service { /* ... */ };
    using resource_system = resource_service; // deprecated
}
```

---

### US-29-3 — Убрать `*_service_init.h/.cpp` файлы

**Файлы:**
- Удалить `res_resource_service_init.h/.cpp`
- Удалить `desc_desc_service_init.h/.cpp`
- Удалить `wnd_window_service_init.h/.cpp`
- Удалить `ecs_ecs_service_init.h/.cpp`
- Удалить `rnd_render_service_init.h/.cpp`
- Удалить `gui_gui_service_init.h/.cpp`
- Удалить `inp_input_service_init.h/.cpp`
- Удалить `scn_scene_service_init.h/.cpp`
- Удалить `edt_editor_init.h/.cpp`
- `core_module.cpp` — inline construct/destruct
- `engine_module.cpp` — inline construct/destruct
- `editor_module.cpp` — inline construct/destruct

**Результат:** конструирование и удаление сервисов — непосредственно в `register_services()` / `shutdown_services()` модуля. Один файл = один модуль = все его сервисы.

---

### US-29-4 — Удалить глобальные указатели и `get_system()`

**Файлы:**
- `core/core/resource/res_system.h` — убрать `extern p_res_system`, `get_system()`
- `core/engine/render/render_system/rnd_render_system.h` — убрать `extern p_render_system`, `get_system()`
- `core/engine/gui/gui_system/gui_system.h` — убрать `extern p_gui_system`, `get_system()`
- Все callsite'ы `res::get_system()`, `rnd::get_system()`, `gui::get_system()`

**Стратегия:** поэтапная замена:
1. Сначала добавить `app_data_storage&` или прямую ссылку на сервис в конструкторы, где это удобно
2. Затем убрать `get_system()` вызовы
3. В последнюю очередь удалить `extern` объявления

**Сложные места:** `res::get_system()` используется в `shader_config::load()` и других low-level местах. Там нужна инъекция зависимости через параметр или контекст.

---

### US-29-5 — Унифицировать input init (убрать `input_reg`)

**Файлы:**
- `core/engine/input/inp_input_service_init.h/.cpp` (до удаления в US-29-3)
- `core/engine/engine_module.cpp`

**Проблема:** input использует `input_reg()` для register phase и `input_init()` для initialize phase. Все остальные используют единый `*_init()`.

**Решение:** объединить в стандартный паттерн — конструирование в `register_services()`, настройка подписок в `initialize_services()`.

---

### US-29-6 — Документация: паттерн добавления нового сервиса

**Файлы:**
- `CLAUDE.md` — обновить секцию "Module System"

**Документировать:**

```
Как добавить новый сервис:

1. Создать класс в нужном слое (core/engine/editor)
   - Суффикс: _service
   - Зависимости — через конструктор

2. В модуле своего слоя:
   register_services():
     data.construct<my::my_service>(deps...);
   shutdown_services():
     data.destruct<my::my_service>();

3. Если есть desc-типы — зарегистрировать в том же register_services()

Всё. Два файла (класс + модуль), одна точка регистрации.
```

---

## Порядок регистрации (целевой)

```
CORE module:
  construct<res::resource_service>()
  construct<desc::desc_service>(res)
  construct<wnd::window_service>()
  construct<ecs::system_factory_service>()

ENGINE module:
  construct<inp::input_service>()
  construct<inp::ecs_input_manager>()
  construct<rnd::render_service>(res, desc)
    → register texture descs
  construct<gui::gui_service>()
  construct<scn::assembler_service>(desc)
    → register scene descs + spawners
  construct<scn::hot_reload_service>(res, assembler)
  construct<scn::level_service>(res, assembler, factory)
  construct<rnd::skinning_service>()
  construct<rnd::frame_assembler_service>()

EDITOR module:
  construct<edt::editor_service>(desc, res, rnd, gui)
  construct<app::app_loop_service_interface, edt::edt_loop_service>()
```

---

## Граф зависимостей

```
US-29-1 (Удалить game_system)  ← первый, standalone
  └── US-29-5 (Унифицировать input init)

US-29-2 (Переименовать сервисы)  ← можно параллельно с US-29-1
  └── US-29-3 (Убрать *_init файлы)
        └── US-29-4 (Убрать глобальные указатели)  ← последний, самый рисковый

US-29-6 (Документация)  ← после всех
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Удалить game_system** | US-29-1, US-29-5 | Регистрации в своих модулях; game_module удалён; input унифицирован |
| **2 — Переименование** | US-29-2 | Все сервисы с `_service` суффиксом; using aliases на переходный период |
| **3 — Убрать boilerplate** | US-29-3 | `*_init.cpp` удалены; construct/destruct inline в модулях |
| **4 — Убрать глобалы** | US-29-4 | Нет `p_*_system`, нет `get_system()`; DI через конструктор |
| **5 — Документация** | US-29-6 | CLAUDE.md обновлён |

---

## Риски

| Проблема | Решение |
|---|---|
| Массивное переименование (US-29-2) | `using` aliases; IDE rename refactoring; поэтапно по модулям |
| `get_system()` в low-level коде (shader_config::load) | Передавать `resource_service&` как параметр; или временно оставить один `get_system()` до рефакторинга рендера |
| Порядок construct/destruct нарушится | Модули уже упорядочены по priority; внутри модуля порядок construct определяет порядок |
| Большой diff | Разбить на 4 фазы; каждая фаза — отдельный коммит с прогоняемыми тестами |

---

## Критерии готовности

- [ ] `game_system` и `game_module` удалены из кодовой базы
- [ ] Все сервисы имеют суффикс `_service`
- [ ] `*_service_init.h/.cpp` файлы удалены; конструирование inline в модулях
- [ ] Нет глобальных указателей `p_*_system`; нет `get_system()` функций
- [ ] Добавление нового сервиса = 2 файла (класс + строка в модуле)
- [ ] CLAUDE.md документирует паттерн
- [ ] Все тесты проходят, сцены загружаются
