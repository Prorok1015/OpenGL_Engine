# EPIC-16: Data Contracts — контракты данных рендера

**Theme:** render-migration
**Status:** in_progress
**Depends on:** нет

## Цель

Обогатить структуры данных `render_packet_t` / `frame_context` до уровня, достаточного для
реализации всех render pass'ов **без обращения к ECS во время рендеринга**.
Render pass должен получать всё необходимое из `frame_context` — без знания о мире сцены.

---

## Контекст

Сейчас `draw_call_t` содержит `geometry_tag`, но не тег материала.
Данные о свете (`directional_light_component`) вычисляются инлайн в `renderer_3d`.
Viewport передаётся в неправильном формате.

---

## US-16-1: Добавить `material_tag` в `draw_call_t`

**Файл:** `core/engine/render/render_system/rnd_render_packet.hpp`
**Приоритет:** КРИТИЧЕСКИЙ

`draw_call_t` содержит `geometry_tag`, но render pass не может выбрать шейдер без тега материала.

```cpp
struct draw_call_t {
    uint64_t  sort_key;
    res::tag  geometry_tag;
    res::tag  material_tag;    // ← ДОБАВИТЬ
    uint32_t  indices_count;
    uint32_t  vx_begin;
    uint32_t  ind_begin;
    glm::mat4 transform;
};
```

Экстрактор заполняет `material_tag` из `geometry_component` (или `mesh_node_desc` после EPIC-12).

**AC:**
- `draw_call_t` содержит `material_tag`
- `scn_render_data_extractor` заполняет `material_tag` при построении draw call

---

## US-16-2: Структуры данных освещения в `rnd_light_data.hpp`

**Файл:** `core/engine/render/render_system/rnd_light_data.hpp` (новый)
**Приоритет:** ВЫСОКИЙ

Данные о свете сейчас вычисляются инлайн в `renderer_3d::prepare_directional_light()`.
Нужны независимые POD-структуры для передачи через `frame_context`.

```cpp
namespace rnd {

struct directional_light_t {
    glm::vec3 direction;
    glm::vec3 color;
    float     intensity;
};

struct scene_lights_t {
    std::vector<directional_light_t> directional;
    glm::vec3                        ambient_color;
    float                            ambient_intensity;
};

} // namespace rnd
```

**AC:**
- Структуры объявлены в отдельном заголовке
- `scene_lights_t` сохраняется в `frame_context` (через `frame_context.data.construct<rnd::scene_lights_t>()`)

---

## US-16-3: Исправить viewport-баг в `scn_render_data_extractor`

**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** КРИТИЧЕСКИЙ

Экстрактор пишет viewport как `{center.x, center.y, size.x, size.y}`.
Render pass ожидает `{left, top, right, bottom}` (как и `glm::ivec4` input rect повсюду в проекте).

```cpp
// Было (НЕПРАВИЛЬНО):
packet.camera.viewport = cam.m_viewport;

// Должно быть:
auto& vp = cam.m_viewport;
packet.camera.viewport = glm::ivec4{
    vp.center.x - vp.size.x / 2,
    vp.center.y - vp.size.y / 2,
    vp.center.x + vp.size.x / 2,
    vp.center.y + vp.size.y / 2
};
```

> Или пересмотреть хранение viewport в `camera_component` — возможно, сразу хранить `{left, top, right, bottom}`.

**AC:**
- Viewport в `render_packet_t` в формате `{left, top, right, bottom}`
- Unit-тест проверяет корректность преобразования

---

## US-16-4: Разделение draw calls на opaque и transparent

**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** US-16-1

Все меши сейчас попадают в `opaque_draws`. Нужно проверять флаг прозрачности материала.

- Загружать/кэшировать `material_desc` по `material_tag`
- Проверять `queue == "transparent"` или `blend_mode`
- Направлять в `transparent_draws` или `opaque_draws` соответственно

**AC:**
- Объекты с прозрачными материалами попадают в `transparent_draws`
- Остальные — в `opaque_draws`

---

## Критерии готовности

- [x] `draw_call_t` содержит `material` (реализовано как `shader_config` — лучше чем `material_tag`)
- [x] `rnd_light_data.hpp` создан, `scene_lights_t` сохраняется в `frame_context`
- [ ] **BUG:** Viewport в `render_packet_t` передаётся как `{cx,cy,w,h}`, а pass'ы ожидают `{left,top,right,bottom}` — fix в `scn_camera_component.hpp` viewport operator
- [x] Draw calls разделены на opaque / transparent
