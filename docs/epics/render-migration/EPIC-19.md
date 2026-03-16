# EPIC-19: Pipeline Wiring — подключение конвейера

**Theme:** render-migration
**Status:** planned
**Depends on:** EPIC-18

## Цель

Зарегистрировать все render pass'ы в `render_system`, подключить новый конвейер к редактору
и убедиться, что `edt_frame_loop_service` рендерит сцену через новую архитектуру end-to-end.

---

## US-19-1: Зарегистрировать render pass'ы в `rnd_render_service_init`

**Файл:** `core/engine/render/render_system/rnd_render_service_init.cpp`
**Приоритет:** КРИТИЧЕСКИЙ

```cpp
// В rnd_render_service_init::initialize_services():
auto& rs = data.require<rnd::render_system>();
auto& drv = data.require<rnd::driver_interface>();

rs.add_render_pass(std::make_shared<rnd::z_prepass>(drv));
rs.add_render_pass(std::make_shared<rnd::opaque_pass>(drv, shader_mgr, geom_mgr));
rs.add_render_pass(std::make_shared<rnd::skybox_pass>(drv));
rs.add_render_pass(std::make_shared<rnd::transparent_pass>(drv));
rs.add_render_pass(std::make_shared<rnd::composition_pass>(drv));
```

Порядок регистрации = порядок выполнения. Должен соответствовать глубинному тесту и blending state.

**AC:**
- `render_system::render_frame(context)` выполняет все проходы в правильном порядке
- Нет null-pointer dereference при пустой `frame_context`

---

## US-19-2: End-to-end верификация редакторского конвейера

**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** US-19-1, EPIC-17 US-17-2

Полный сквозной тест: редактор загружает уровень → `build_frame` → `render_frame` → viewport отображает сцену.

**Чеклист:**
- [ ] `edt_frame_loop_service::on_step()` вызывает `build_frame` с данными в `frame_context`
- [ ] `render_system::render_frame(context)` запускает все pass'ы без ошибок
- [ ] Viewport редактора отображает сцену без артефактов
- [ ] Прозрачные объекты рендерятся корректно
- [ ] Skybox отображается
- [ ] FPS не хуже чем до миграции (±10%)

**AC:**
- Все пункты чеклиста выполнены
- В логах нет ошибок OpenGL (проверить через `glGetError` или debug callback)

---

## Критерии готовности

- [ ] Все pass'ы зарегистрированы и выполняются в правильном порядке
- [ ] Редакторский конвейер (`edt_frame_loop_service`) рендерит сцену через новую архитектуру
- [ ] Визуальный результат совпадает со старым `renderer_3d`
- [ ] Нет OpenGL ошибок в логах
