# EPIC-18: Render Passes — конкретные проходы рендеринга

**Theme:** render-migration
**Status:** planned
**Depends on:** EPIC-17

## Цель

Реализовать все графические проходы, полностью покрывающие функциональность
`scn::renderer_3d::on_render()`. После этого эпика `renderer_3d` можно отключить.

**Стратегия:** каждый pass — прямой порт соответствующего блока из `renderer_3d::on_render()`.
Шейдеры не меняем — только перемещаем вызовы драйвера в изолированные классы.

**Расположение файлов:** `core/engine/render/render_system/passes/`

---

## US-18-1: `rnd::z_prepass` — проход глубины

**Файлы:** `passes/rnd_z_prepass.h/.cpp` (новые)
**Приоритет:** ВЫСОКИЙ

Первый проход — запись только глубины без записи цвета.
Порт из `renderer_3d::z_prepass()`.

```cpp
class z_prepass : public render_pass_interface {
public:
    void execute(frame_context& ctx, driver::driver_interface& drv) override;
};
```

- Получить `vector<render_packet_t>` из `ctx`
- Для каждого пакета: установить viewport, активировать `z_prepass.vert/frag`
- Итерировать `opaque_draws`, вызывать `drv.draw_indexed()`
- Depth write ON, color write OFF

**AC:**
- Depth buffer заполняется после прохода
- Color buffer не изменяется

---

## US-18-2: `rnd::opaque_pass` — основной проход

**Файлы:** `passes/rnd_opaque_pass.h/.cpp` (новые)
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** US-18-1

Основной проход — рендеринг непрозрачной геометрии с phong-освещением.
Порт из `renderer_3d::render_scene()`.

- Получить `vector<render_packet_t>` и `scene_lights_t` из `ctx`
- Резолвить `material_tag` → шейдерная программа через `shader_manager`
- Резолвить `geometry_tag` → VAO через `geom_manager`
- Загружать uniforms: model/view/projection matrices, light data, camera pos
- Вызывать `drv.draw_indexed()` для каждого `draw_call_t` в `opaque_draws`

**Сортировка draw calls (GPU performance):**
```
sort_key = (shader_id << 32) | (geometry_id & 0xFFFFFFFF)
```

Применить `std::sort` к `opaque_draws` по `sort_key` перед рендерингом.

**AC:**
- Непрозрачные объекты отображаются с phong-освещением
- Нет зависаний при пустом `opaque_draws`

---

## US-18-3: `rnd::skybox_pass` — проход skybox

**Файлы:** `passes/rnd_skybox_pass.h/.cpp` (новые)
**Приоритет:** СРЕДНИЙ
**Зависимости:** US-18-2

Рендеринг skybox cubemap. Порт из соответствующего блока в `renderer_3d`.
Данные о skybox (cubemap tag) нужно добавить в `frame_context` — либо отдельным полем,
либо через `scn::skybox_extractor`.

- Отключить depth write, depth test — `GL_LEQUAL`
- Трансформация view без трансляции (убрать translation из view matrix)
- Шейдеры: `sky.vert` / `sky.frag`

**AC:**
- Skybox отображается за всей геометрией
- Горизонт не "прыгает" при движении камеры

---

## US-18-4: `rnd::transparent_pass` — OIT weighted blending

**Файлы:** `passes/rnd_transparent_pass.h/.cpp` (новые)
**Приоритет:** СРЕДНИЙ
**Зависимости:** US-18-2, EPIC-16 US-16-4

OIT проход (Order-Independent Transparency) — weighted blending по McGuire & Bavoil.
Порт из `renderer_3d::render_transparent()`.

- Настроить accumulation + revealage render targets
- Рендерить `transparent_draws` с шейдерами `transparent.vert/frag`
- Depth write OFF, специальный additive blending

**AC:**
- Прозрачные объекты (стекло, частицы) рендерятся без артефактов сортировки
- Accumulation и revealage буферы заполнены корректно

---

## US-18-5: `rnd::composition_pass` — финальная композиция

**Файлы:** `passes/rnd_composition_pass.h/.cpp` (новые)
**Приоритет:** СРЕДНИЙ
**Зависимости:** US-18-4

Финальный fullscreen quad pass — смешивает opaque и OIT результаты.
Порт из `renderer_3d::compose()`.

- Шейдеры: `composition.vert/frag` (или `mix_opaque_trans_scene.vert/frag`)
- Результат пишется в итоговый render target (`__editor_camera_rt` для редактора)

**AC:**
- Финальное изображение включает правильно скомпозированную прозрачность

---

## US-18-6: `rnd::normal_debug_pass` — отладочный проход нормалей

**Файлы:** `passes/rnd_normal_debug_pass.h/.cpp` (новые)
**Приоритет:** НИЗКИЙ
**Зависимости:** US-18-2

Отображение нормалей через geometry shader. Активируется через флаг в редакторе.
Шейдеры: `normal.vert/geom/frag`.

**AC:**
- Нормали отображаются только когда флаг активен
- Pass корректно пропускается если флаг выключен

---

## Критерии готовности

- [ ] Все 5 основных pass'ов реализованы (`z_prepass`, `opaque`, `skybox`, `transparent`, `composition`)
- [ ] `normal_debug_pass` реализован (опционально, низкий приоритет)
- [ ] Каждый pass получает данные только из `frame_context`, без прямого обращения к ECS
- [ ] Визуально результат совпадает с `renderer_3d::on_render()` при одинаковой сцене
