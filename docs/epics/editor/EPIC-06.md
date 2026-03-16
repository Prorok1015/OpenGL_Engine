# EPIC-06: Рефакторинг и чистка кодовой базы редактора

**Theme:** editor
**Status:** done
**Depends on:** EPIC-08, EPIC-09, EPIC-10, EPIC-11 (большая часть рефакторинга
  логически следует после перехода на desc-driven)

## Цель

Устранить технический долг, накопившийся в первых итерациях редактора.
Привести код к конвенциям проекта, убрать дублирование.

---

## US-06-1: Удалить `edt_scene_serializer.hpp`

После реализации US-25 (simplified save) файл становится мёртвым кодом.
- Удалить `edt_scene_serializer.hpp`
- Убедиться, что нигде больше не используется

---

## US-06-2: Убрать `registry_sp` из `editor_system`

После EPIC-08/10/11 `shared_ptr<entt::registry> registry_sp` заменяется на
`world_desc* m_world_desc`. Убрать поле и все использования.
`viewport_panel` оставляет ссылку на registry read-only (для камеры).

---

## US-06-3: Привести панели к единому интерфейсу

После EPIC-10 панели имеют разные источники данных:
- `hierarchy_panel` → `world_desc*`
- `inspector_panel` → `prefab_node*`
- `viewport_panel` → read-only `registry&` (только камера и гизмо)

Убедиться что `panel_base` и `panel_manager` не хранят ECS-специфичные ссылки.

---

## US-06-4: Консолидация include в `edt_editor_system.h`

Заголовок `edt_editor_system.h` включает много ECS/панельных заголовков.
После рефакторинга — убрать ненужные includes, заменить на forward declarations
где возможно.

---

## US-06-5: `edt_component_renderers.hpp` — разбить на файлы

Сейчас все рендереры в одном `.hpp`. После EPIC-10 рендереры работают с
`prefab_comp_node` — вынести каждый тип в отдельный файл:
```
edt_component_renderers/
  edt_cr_camera.cpp
  edt_cr_light.cpp
  edt_cr_skybox.cpp
  edt_cr_skin.cpp
  edt_cr_transform.cpp
```

---

## US-06-6: Удалить мёртвые includes и forward declarations

Проверить все файлы редактора на неиспользуемые include/forward declarations
(актуально после серии удалений в предыдущих эпиках).

---

## US-06-7: Убрать глобальный доступ к сервисам через `::get_system()`

Все вызовы `res::get_system()`, `rnd::get_system()`, `gui::get_system()` в редакторе
заменить на локальные ссылки, передаваемые через конструктор.

**Затронутые файлы:**
- `edt_editor_system.h/.cpp` — добавить `res::resource_system& m_res`, `rnd::render_system& m_rnd`;
  конструктор принимает их напрямую; убрать вызовы сервисов из конструктора (перенести в `init()`)
- `edt_viewport_panel.h/.cpp` — конструктор принимает `rnd::render_system&`, `gui::gui_system&`
- `edt_editor_init.cpp` — передать сервисы в конструктор `editor_system`

`edt_spawn_system` — отдельная итерация (ECS-система, другой lifecycle).

---

## Критерии готовности

- [x] Нет вызовов `::get_system()` вне ECS-систем в редакторе (`edt_spawn_system` удалён)
- [x] `edt_scene_serializer.hpp` удалён
- [x] `registry_sp` убран из `editor_system` (было сделано в EPIC-08/09)
- [x] Нет ECS-специфичных полей в `panel_base` и `panel_manager`
- [x] `edt_component_renderers` разбит на отдельные файлы под `edt_component_renderers/`
- [x] Нет неиспользуемых includes в заголовках редактора (`scn_model.h`, `inp_input_system.h` удалены из `.h`)
