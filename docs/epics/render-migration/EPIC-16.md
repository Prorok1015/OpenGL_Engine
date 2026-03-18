# EPIC-16: Data Contracts — контракты данных рендера

**Theme:** render-migration
**Status:** done
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

## US-16-3: Исправить семантический viewport-баг в render pass'ах

**Файлы:** `rnd_render_packet.hpp`, `rnd_opaque_pass.cpp`, `rnd_z_prepass.cpp`, `rnd_transparent_pass.cpp`, `rnd_composition_pass.cpp`, `rnd_skybox_pass.cpp`
**Приоритет:** СРЕДНИЙ (функционально работало при viewport от (0,0), но семантика была неверной)

**Проблема:** `scn::viewport` хранит `{lefttop, size}` и operator `glm::ivec4()` возвращает `{left, top, width, height}`. Все 5 render pass'ов вычисляли `viewport.z - viewport.x` (т.е. `width - left`), полагая что формат `{left, top, right, bottom}`. При `left=0` математика случайно давала верный результат, но при ненулевом offset viewport сломался бы.

**Исправление:** зафиксирован контракт `camera_render_data_t::viewport` = `{left, top, width, height}`. Во всех pass'ах убрано ошибочное вычитание — `.z`/`.w` используются напрямую как width/height.

**AC:**
- [x] `camera_render_data_t::viewport` задокументирован как `{left, top, width, height}`
- [x] Все render pass'ы используют `.z`/`.w` напрямую без вычитания

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
- [x] Viewport контракт зафиксирован: `camera_render_data_t::viewport` = `{left, top, width, height}`. Render pass'ы исправлены — убрано ошибочное вычитание `viewport.z - viewport.x`, теперь `.z`/`.w` используются напрямую как width/height
- [x] Draw calls разделены на opaque / transparent
