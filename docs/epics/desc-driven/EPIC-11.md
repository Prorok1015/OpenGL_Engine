# EPIC-11: Editor Camera & Render Targets

**Theme:** desc-driven
**Status:** done
**Depends on:** EPIC-08
**Blocks:** —

## Цель

Редакторская камера живёт вне `world_desc`, не сбрасывается при hot-reload,
рендерит в свой выделенный render target.
`viewport_panel` отображает этот render target, а не `__color_scene_rt`.

---

## Контекст: render target в camera_component

`camera_component.texture` (`res::tag`) и `camera_desc.m_texture` (`res::tag`)
уже существуют — они указывают, в какую текстуру рендерить.
Render data extractor строит `render_packet_t` с `packet.camera.target_texture_tag`.
Renderer пишет в эту текстуру.

Сейчас все камеры пишут в `memory://__color_scene_rt`.
После рефакторинга у каждой камеры будет свой тег.

---

## US-29: `editor_camera_tag` + `editor_camera_state`

**Файлы:** `Editor/code/editor_system/edt_editor_camera.h` (новый)

```cpp
#pragma once
#include "res_tag.h"
#include <glm/glm.hpp>

namespace edt {

    // Маркер — пустой компонент, отличает editor camera entity от scene cameras
    struct editor_camera_tag {};

    // Персистентное состояние (переживает hot-reload)
    struct editor_camera_state {
        glm::vec3 position      { 0.f, 2.f, 5.f };
        glm::vec3 rotation      { 0.f };           // radians
        float     distance      = 0.f;
        float     movement_speed  = 0.05f;
        float     rotating_speed  = 0.005f;
        float     fov             = 60.f;
        float     near_distance   = 0.01f;
        float     far_distance    = 1000.f;

        // Тег рендер-таргета редакторской камеры
        static constexpr std::string_view RT_TAG = "memory://__editor_camera_rt";
    };

} // namespace edt
```

**В `editor_system`:**

```cpp
edt::editor_camera_state m_camera_state;
```

---

## US-30: Инжекция editor camera после hot-reload

**Файлы:** `edt_editor_system.cpp`

### `save_editor_camera_state(registry&)`

Вызывается ДО reload мира — сохраняет актуальное положение:

```cpp
void save_editor_camera_state(entt::registry& reg) {
    for (auto [ent, ctrl] :
         reg.view<edt::editor_camera_tag, scn::mouse_controller_component>().each()) {
        m_camera_state.position = ctrl.position;
        m_camera_state.rotation = ctrl.rotation;
        m_camera_state.distance = ctrl.distance;
        break;
    }
}
```

### `inject_editor_camera(registry&)`

Вызывается ПОСЛЕ reload мира:

```cpp
void inject_editor_camera(entt::registry& reg) {
    // Убрать старую если осталась (на случай повторного вызова)
    for (auto ent : reg.view<edt::editor_camera_tag>())
        reg.destroy(ent);

    entt::entity cam = reg.create();
    reg.emplace<edt::editor_camera_tag>(cam);

    // camera_component с выделенным render target
    scn::camera_component cam_comp;
    cam_comp.fov           = m_camera_state.fov;
    cam_comp.near_distance = m_camera_state.near_distance;
    cam_comp.far_distance  = m_camera_state.far_distance;
    cam_comp.texture       = res::tag{ edt::editor_camera_state::RT_TAG };
    reg.emplace<scn::camera_component>(cam, cam_comp);

    // Контроллер — хранит позицию/поворот для update_camera_matrix_system
    scn::mouse_controller_component ctrl;
    ctrl.position       = m_camera_state.position;
    ctrl.rotation       = m_camera_state.rotation;
    ctrl.distance       = m_camera_state.distance;
    ctrl.movement_speed = m_camera_state.movement_speed;
    ctrl.rotating_speed = m_camera_state.rotating_speed;
    reg.emplace<scn::mouse_controller_component>(cam, ctrl);

    reg.emplace<scn::local_transform>(cam);
    reg.emplace<scn::world_transform>(cam);
}
```

### Watcher — полный поток

```cpp
// В on_editor_world_reloaded():
void on_editor_world_reloaded() {
    auto lvl_mgr = m_lvl_manager.lock();
    if (!lvl_mgr) return;
    auto& world = lvl_mgr->get_level().get_world(m_active_world_name);

    save_editor_camera_state(world.state());   // сохранить до пересоздания
    // (hot-reload уже выполнился — world.state() уже новый registry)
    inject_editor_camera(world.state());

    // re-find selected node by name в m_world_desc
    re_select_node_after_reload();

    // Обновить панели
    m_hierarchy_panel->set_world_desc(&m_world_desc);
    m_viewport_panel->set_registry(
        std::shared_ptr<entt::registry>(&world.state(), [](auto*){}));
}
```

> **Важно:** `save_editor_camera_state` вызывается из watcher'а уже ПОСЛЕ того
> как `level::reload_world` пересоздал registry. Поэтому состояние нужно
> сохранять ещё до вызова `signal_changed`. Порядок:
> 1. `save_editor_camera_state(old_reg)` — перед `serialize_and_push`
> 2. `serialize_and_push(...)` → hot-reload → watcher вызывает `on_editor_world_reloaded`
> 3. `inject_editor_camera(new_reg)`

---

## US-31: `render_data_extractor` — приоритет editor camera

**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp`

### Проблема

В мире могут быть и editor camera entity, и scene camera (из `camera_desc` в world_desc).
Extractor создаст два render_packet → некорректный рендер.

### Решение

```cpp
for (uint32_t i = 0; i < level.get_world_count(); ++i) {
    auto& reg = level.get_world(i).state();

    bool has_editor_cam = !reg.view<edt::editor_camera_tag>().empty();

    for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
        // В edit mode используем только editor camera
        if (has_editor_cam && !reg.all_of<edt::editor_camera_tag>(ent))
            continue;

        // ... остальной код без изменений
    }
}
```

> `edt::editor_camera_tag` — runtime-only компонент из editor layer.
> `scn_render_data_extractor.cpp` находится в engine layer.
> **Решение зависимости:** либо вынести `editor_camera_tag` в core/common
> (как пустой маркер без зависимостей от edt), либо использовать
> `res::tag` в `camera_component.texture` для идентификации
> (editor camera имеет специфичный RT тег — проверяем по нему).

**Рекомендуемый вариант:** проверка по `camera_component.texture`:

```cpp
static const res::tag EDITOR_RT = res::tag{ edt::editor_camera_state::RT_TAG };

bool has_editor_cam = false;
for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
    if (cam.texture == EDITOR_RT) { has_editor_cam = true; break; }
}

for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
    if (has_editor_cam && cam.texture != EDITOR_RT) continue;
    // ...
}
```

Это позволяет engine layer не знать о `edt` namespace.

---

## US-32: `viewport_panel` — отображать editor camera RT

**Файлы:** `edt_viewport_panel.cpp`

### Сейчас

```cpp
static const res::tag color_rt_tag = res::tag(res::tag::memory, "__color_scene_rt");
auto* texture = rnd::get_system().get_texture_manager().find(color_rt_tag);
```

### После

```cpp
static const res::tag editor_rt_tag = res::tag{ edt::editor_camera_state::RT_TAG };
auto* texture = rnd::get_system().get_texture_manager().find(editor_rt_tag);
```

Viewport читает текстуру по тегу editor camera RT.
Gizmo и orientation gizmo ищут камеру по `camera_component.texture == EDITOR_RT`.

---

## Критерии готовности

- [ ] `inject_editor_camera()` создаёт entity с `camera_component.texture = EDITOR_RT`
- [ ] После hot-reload editor camera state (позиция/поворот) сохраняется
- [ ] `render_data_extractor` игнорирует scene cameras при наличии editor camera
- [ ] `viewport_panel` отображает render target `EDITOR_RT`, не `__color_scene_rt`
- [ ] Gizmo использует матрицы editor camera
- [ ] Scene camera (`camera_desc` в world_desc) не мешает рендеру в edit mode
- [ ] Input через `update_camera_matrix_system` работает для editor camera entity
