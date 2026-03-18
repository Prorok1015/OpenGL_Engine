# EPIC-17: Extractor Layer — слой экстракторов

**Theme:** render-migration
**Status:** done
**Depends on:** EPIC-16

## Цель

Полностью перенести логику **сбора данных** из `renderer_3d` в независимые `extractor_interface`.
После этого эпика `frame_assembler::build_frame()` заполняет `frame_context` всеми данными,
необходимыми render pass'ам: геометрия, освещение, камера.

---

## Контекст

Сейчас:
- `scn_render_data_extractor` частично реализован, с viewport-багом (исправлен в EPIC-16)
- `light_extractor` отсутствует — свет читает только `renderer_3d` инлайн
- `frame_assembler` создаётся нигде — экстракторы к нему не подключены

---

## US-17-1: Реализовать `scn::light_extractor`

**Файлы:** `core/engine/scene/level/scn_light_extractor.h/.cpp` (новые)
**Приоритет:** ВЫСОКИЙ
**Зависимости:** EPIC-16 US-16-2

Читает `directional_light_component` из ECS, заполняет `rnd::scene_lights_t` в `frame_context`.

```cpp
class light_extractor : public rnd::extractor_interface {
public:
    explicit light_extractor(scn::level_manager& lm) : m_lm(lm) {}
    void extract(rnd::frame_context& ctx) override;
private:
    scn::level_manager& m_lm;
};
```

`extract()` итерирует все worlds (или только активный), читает `directional_light_component`,
конвертирует в `directional_light_t`, сохраняет результат в `ctx.data`.

**AC:**
- После `build_frame` в `frame_context` доступны `scene_lights_t`
- Светильники без `light_component` не попадают в структуру

---

## US-17-2: Зарегистрировать `frame_assembler` и экстракторы

**Файлы:** `core/engine/render/render_system/rnd_render_service_init.cpp`
          (или `core/engine/scene/scn_scene_service_init.cpp`)
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** US-17-1

`frame_assembler` существует как класс, но нигде не создаётся и не регистрируется в `app_data_storage`.
Экстракторы к нему не подключены.

```cpp
// В initialize_services():
auto& lm = data.require<scn::level_manager>();
auto& fa = data.construct<rnd::frame_assembler>();
fa.add_extractor(std::make_shared<scn::render_data_extractor>(lm));
fa.add_extractor(std::make_shared<scn::light_extractor>(lm));
```

`edt_frame_loop_service` получает `frame_assembler` через `data.require<rnd::frame_assembler>()`.

**AC:**
- `frame_assembler` регистрируется в `app_data_storage`
- `edt_frame_loop_service::on_step()` успешно вызывает `build_frame`
- `frame_context` после `build_frame` содержит не пустые `render_packet_t` и `scene_lights_t`

---

## US-17-3: Frustum culling в `render_data_extractor` (оптимизация)

**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** НИЗКИЙ
**Зависимости:** EPIC-16 US-16-3

Отсекать draw calls вне camera frustum до генерации `draw_call_t`.
Требует вычисления frustum planes из view-projection матрицы камеры.

**AC:**
- Объекты за камерой не попадают в `opaque_draws`
- Производительность в сценах с большим числом объектов улучшается (можно проверить по счётчику draw calls)

---

## Критерии готовности

- [x] `scn::light_extractor` реализован и регистрирует свет в `frame_context`
- [x] `frame_assembler` создаётся и регистрируется при инициализации (`scn_scene_service_init.cpp`)
- [x] `edt_frame_loop_service::on_step()` вызывает `build_frame` с рабочими экстракторами
- [x] `frame_context` после `build_frame` содержит корректные данные геометрии и освещения
- [ ] US-17-3 frustum culling — не реализован (низкий приоритет, skip)
