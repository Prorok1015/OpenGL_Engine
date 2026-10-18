# Дизайн: цикл кадра и цель рендера камеры (эпик #9)

**Issue:** #9 (stories #96–#100) · **Утверждено владельцем:** 2026-10-18 («общий цикл со стадиями и камера с целью рендера»)

## 1. Как устроено сейчас

- Стадии кадра — `gs::frame_stage_registry` (`core/engine/game_system/gs_frame_stage.h:24`), порядок по числовому `order` (`stable_sort`): 5 сброс frame-аллокатора, 10 события окна, 20 `level_manager::update`, 28 `render_system::begin_frame`, 30 `frame_assembler::build_frame`, 40 `render_frame`, 50 `process_windows` (swap); редактор добавляет 25 (сброс дельт ввода) и 45 (ImGui) — `Editor/code/editor_system/core/edt_frame_loop_service.cpp:30,35`.
- Два почти одинаковых loop-сервиса: `edt::edt_loop_service` и `gs::gs_loop_service` (тайминг, `dt`, контекст кадра, запуск стадий скопированы). Создаётся только редакторский (`edt_editor_init.cpp:18`), игровой путь мёртв.
- `loop.init()` уже в `application::run` (`core/core/application/application.cpp:20`), симметричного `term` нет.
- Пассы рисуют в цель камеры (`rnd::resolve_color_target(packet.camera)`), картинку показывает только ImGui-вьюпорт редактора; вывода в backbuffer нет.

## 2. Решение

**Камера — то, что реально показывается.** У камеры есть цель рендера: `backbuffer` (по умолчанию) или текстура. В игре камера рисует прямо в backbuffer. Редактор при старте переназначает цели игровых камер на текстуры, показывает их во вьюпорте и добавляет свою редакторскую камеру. Отдельного present-пасса нет.

**Общий цикл, хост-специфичное — стадиями.** Один `app::loop_service` в core. Редакторские вещи (ImGui, редакторская камера, переназначение целей) добавляет `editor_module` своими стадиями; без него их нет. Движок о редакторе не знает.

**Именованные стадии** (зеркало фаз ECS, `core/core/ecs/include/snk/ecs/ecs_phase.hpp`): `begin, input, update, post_update, extract, render, ui, present`. Встроенные нельзя переставлять/удалять; модуль добавляет свою стадию `before/after` встроенной; при равном якоре — порядок регистрации; дубль имени отклоняется. Соответствие: 5→begin, 10→input, 20→update, 25→post_update, 28+30→extract, 40→render, 45→ui, 50→present.

## 3. Решения (по рекомендации)

1. Реестр стадий и `loop_service` — в core (`core/core/application/`); содержимое стадий с `rnd::`/`wnd::` регистрирует engine (`begin` — core).
2. Контекст стадии в core — `{storage, dt}`; `rnd::frame_context` живёт в storage.
3. Бывшая стадия 28 (подготовка рендера) — часть `extract`, без отдельного якоря.
4. Цель «backbuffer» проверяется логическими тестами + headless-скриншотом.
5. `gs::` массово не переименовывается; новый код — в `app::`, `gs_frame_stage.*` и `gs_loop_service.*` удаляются.

## 4. Шаги (каждый собирается и проходит тесты)

1. Именованные стадии в реестре (#96), тесты порядка/якорей/дублей.
2. Реестр и контекст стадии переезжают в core без engine-типов.
3. Единый `app::loop_service` (#97); `edt_loop_service`/`gs_loop_service` удаляются; редактор добавляет только свои стадии.
4. `init → step… → term` в `application::run`, `term` и при исключении (#100), тесты.
5. Engine работает без редактора (#98): тест `core + engine` без `editor_module` прогоняет кадр.
6. Цель рендера камеры (#99): `backbuffer | texture`, по умолчанию backbuffer; редактор переназначает игровые камеры на текстуры; без редактора камера рисует в default framebuffer. Тесты выбора цели + headless-скриншот редактора (без регрессии).

Исполняемый player — EPIC-54, здесь не делается.
