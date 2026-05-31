# EPIC-49: Engine Componentization & Module Wiring — чистое разделение на компоненты

**Status:** in_progress (Фаза 0 завершена)
**Theme:** infrastructure
**Depends on:** EPIC-41 (CMake hygiene — частично сделан), EPIC-29 (Service Layer) — координируется

> Источник: мульти-агентный ресёрч компонентизации/связывания (2026-05-31, 4 агента). Сходящиеся выводы.

> **Прогресс (2026-05-31): Фаза 0 «Фундамент» завершена.**
> - **US-49-2** (структурные дефекты) DONE: `add_subdirectory(core)` поднят в root (это был ЕДИНСТВЕННЫЙ
>   путь, вносящий engine в билд — перенесён, не удалён); editor_system убрал мёртвый
>   `target_link_directories` + `PUBLIC engine`→`PRIVATE`; `common` INTERFACE→STATIC, вобрал 5 `mem_*.cpp`.
> - Совместно с EPIC-41 срезом 2: **US-41-1** (ThirdParty.cmake + slim Boost), **US-41-6** (imgui-таргет),
>   **US-41-4** (visibility + `snk::` алиасы на 22 таргетах) — см. EPIC-41.
> - Коммиты `1441bd6 ca6aef9 cedc10b 2d89688 2714c1e`. Verify: чистая сборка preset dev-debug + свежий
>   unit_tests.exe = 307 зелёных, exit 0.
> - Находки: imgui — сабмодуль без CMakeLists → таргет в parent-tracked `cmake/imgui.cmake`. `application`
>   линкует Boost::json, но НЕ использует → PRIVATE, кандидат на удаление.
> - ⚠️ Метод верификации: документированный `cmd /c "...vcvars... && cmake"` ломается в Git Bash
>   (мангл `/c`) → cmake не запускался, тесты гоняли СТАРЫЙ бинарь. Чинить `.bat`-хелпером + `cmd //c`.
> Следующее: Фаза 1 — US-49-4 (entt/imgui fwd-decl, res::tag без json/glm).

---

## Мотивация

Проект собирается и работает, но **границы компонентов существуют только на бумаге**. Пока модулей
немного — терпимо; при росте (audio/physics/новый render-backend/editor-tools) текущая структура
начнёт «стрелять в ногу»: любой файл может включить любой внутренний заголовок любого слоя, тяжёлые
зависимости текут вверх, scene и render сварены намертво. Эпик закрывает структурные пробелы, НЕ
покрытые EPIC-41 (гигиена CMake) и EPIC-29 (сервисный слой) — делать СЕЙЧАС, до активного роста.

### Что уже покрыто (не дублируем)
- EPIC-41: visibility specifiers (US-41-4), imgui-таргет (US-41-6), assimp из scene/resource (US-41-7),
  ThirdParty.cmake (US-41-1), snk:: алиасы (US-41-4), warnings (US-41-10).
- EPIC-29: удалить game_system (US-29-1), inline construct/destruct (US-29-3), убрать globals/get_system (US-29-4).

### Net-new пробелы (этот эпик)
1. **Нет публичных API-границ:** каждый таргет делает `target_include_directories(PUBLIC <корень исходников>)`
   → потребитель видит ВСЕ внутренние заголовки. Границ компонентов нет.
2. **scene↔render сварены:** `scn::material_desc` встраивает `rnd::texture_manager&`,
   `rnd::scene_shader_desc::constant_data`, `rnd::driver::RENDER_MODE` в публичные desc-заголовки
   (`scn_material_desc.h`, `scn_model.h`, `scn_mesh_node_desc.h`); scene и все 6 render-пассов берут
   менеджеры через синглтон `rnd::get_system()` вместо инъекции.
3. **Тяжёлые заголовки текут:** `res::tag` тащит `<boost/json.hpp>`+`glm` в 33 файла; `desc_base.hpp`
   json в 25; `entt/entt.hpp` (полный) и `imgui.h` в публичных заголовках.
4. **Связывание модулей неявное:** порядок init = порядок строк (нет `depends_on()`); register НЕ
   сортируется по priority; god-оркестратор кадра захардкожен и продублирован (edt/gs loop).
5. **Нарушения слоёв:** `window` → `gui_gl_backend` (нижний слой зависит от верхнего); `scene` → `gui`.
6. **Структурные дефекты сборки:** `editor_system` повторно конфигурирует core через
   `add_subdirectory(${ENGINE_CORE_PATH})`; `common` — не настоящая либа (mem_*.cpp вкомпилены в `core`);
   `engine`/`core`-зонтики экспортируют всё через PUBLIC.

---

## Целевая компонентная модель

Четыре строгих слоя, зависимости только вниз; внутри слоя — только через публичный API компонента.

```
LAYER 4  EDITOR/GAME    editor_system        (game_module DLL — EPIC-42)
                          → snk::engine_api (только заголовки)
LAYER 3  ENGINE         render  scene  gui  input_system  asset_import
                          агрегатор snk::engine (PRIVATE всё) + snk::engine_api (INTERFACE)
                          → snk::core_api
LAYER 2  CORE           ecs  resource  desc  window  input_core  cfg  drv_opengl  application
                          агрегатор snk::core (PRIVATE всё) + snk::core_api (INTERFACE)
                          → snk::common
LAYER 1  COMMON         common(+mem)  engine_logger  config   +  обёртки third-party (imgui/...)
```

**Правила:** PRIVATE по умолчанию; PUBLIC только если тип зависимости в публичных заголовках; third-party
PRIVATE/за фасадом (исключения: EnTT, Boost::json — где реально в API); запрет восходящих/циклических
зависимостей, проверяемый компиляцией (компонент собирается в изоляции только со своими PRIVATE-deps).

**Образцы для подражания (уже есть в проекте):** `rnd::driver::driver_interface` (GL полностью изолирован
forward-декларациями), `rnd::extractor_interface` (чистый шов scene→render через frame_context),
`app::app_loop_service_interface`, `gui::imgui_backend_interface`.

---

## User Stories

### US-49-1 — Публичные API-границы компонентов (public/ + private include)
**Файлы:** все `*/CMakeLists.txt` с `target_include_directories(... PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})`.
Ввести на компонент публичный «конверт» (`public/` заголовки) и держать внутренние рядом с .cpp;
`target_include_directories(<c> PUBLIC public PRIVATE src)`. На первом этапе допустимо не разносить
физически директории, а сузить, что реально публично. Снимает «всё видно всем».
**AC:** ни один таргет не открывает корень исходников как PUBLIC; потребитель компилируется только с его
публичными заголовками; сборка + 307 тестов зелёные. Усилие: **L**.

### US-49-2 — Исправить структурные дефекты сборки ✅ DONE (2026-05-31, `1441bd6`)
**Файлы:** `Editor/code/editor_system/CMakeLists.txt` (удалить `add_subdirectory(${ENGINE_CORE_PATH})`
стр.19 и бессмысленный `target_link_directories` стр.7; `PUBLIC engine`→`PRIVATE` — координир. с EPIC-41 US-41-4);
`core/core/CMakeLists.txt` (вынести mem_*.cpp в `common`); `core/core/common/CMakeLists.txt` (`common`→STATIC).
**AC:** `common` — самодостаточная STATIC-либа с аллокаторами; editor_system не реконфигурирует core; сборка зелёная. Усилие: **M**.

### US-49-3 — `snk::engine_api` / `snk::core_api` INTERFACE-контракты
**Файлы:** `core/engine/CMakeLists.txt`, `core/core/CMakeLists.txt`, новые INTERFACE-таргеты.
Агрегаторы `engine`/`core` — PRIVATE всё; публичный контракт слоя — отдельный INTERFACE-таргет с публичными
заголовками. editor_system и будущий game_module линкуются к `engine_api`, не к зонтику.
**AC:** editor линкуется к `snk::engine_api`; `engine`/`core` не экспортируют impl-зависимости. Усилие: **M**.

### US-49-4 — Изоляция тяжёлых заголовков
**Файлы:** `core/core/resource/res_tag.h` (вынести `tag_invoke`+`<boost/json.hpp>` в `res_tag_json.h`;
заменить glm::ivec2-поля на POD), `core/core/desc/desc_base.hpp` (json в .cpp где можно),
`scn_world.h`/`scn_ecs_assembler.h`/`scn_hot_reload_manager.h` (`entt/entt.hpp`→`entt/fwd.hpp`),
`gui_backend_interface.h`+editor-панели (`imgui.h`→forward-decl `ImTextureID`).
**AC:** `res::tag`/`desc_base` не тянут boost.json в потребителей; заголовки используют entt/imgui fwd-decl;
заметное снижение времени компиляции. Усилие: **M**.

### US-49-5 — Развязать scene→render: POD-материалы + интерфейс провайдера
**Файлы:** `core/engine/scene/scn_material_desc.h`, `scn_model.h`, `scn_mesh_node_desc.h`,
`scn_render_data_extractor.cpp`; render-сторона (новый `rnd::material_compiler_interface`/`resource_provider`).
Убрать конкретные render-классы из scene-desc заголовков; scene хранит POD-данные материала (цвета/теги/очередь);
`get_shader_desc` перенести на render-сторону (расширение экстрактора).
**AC:** scene-desc заголовки не включают `rnd_texture_manager.h`/`rnd_scene_shader_desc.h`/`rnd_geometry_desc.h`;
scene компилируется без знания внутренностей render; рендер сцен не изменился. Усилие: **L**.

### US-49-6 — Инъекция render-менеджеров через frame_context (убрать rnd::get_system из scene/пассов)
**Файлы:** `scn_render_data_extractor.cpp:61`, `scn_skin_weights_desc.cpp:52`, все 6 `render_system/passes/*.cpp`,
`rnd_render_system.cpp` (положить менеджеры в `frame_context` в начале кадра).
**AC:** ни scene-экстрактор, ни render-пассы не вызывают `rnd::get_system()` — менеджеры берутся из
frame_context; координируется с EPIC-29 US-29-4. Усилие: **M**.

### US-49-7 — Декларация зависимостей модуля + топосорт init
**Файлы:** `core/core/application/app_module_interface.h` (добавить `depends_on()`),
`app_module_loader.hpp` (одна топосортировка; тот же порядок для register И initialize; reverse — shutdown).
**AC:** порядок инициализации выражен декларативно, не порядком строк; register и initialize в одном порядке;
priority-enum заменён/дополнен зависимостями. Усилие: **S/M**.

### US-49-8 — Реестр стадий кадра вместо god-on_step
**Файлы:** `Editor/code/editor_system/core/edt_frame_loop_service.cpp`, `core/engine/game_system/gs_loop_service.cpp`,
новый `frame_stage`-реестр.
Заменить захардкоженный список стадий на упорядоченный реестр (сервисы регистрируют свои стадии как уже
регистрируют render-pass'ы/extractor'ы). Устранить дублирование edt/gs loop.
**AC:** добавление per-frame подсистемы = одна `add_stage`, без правки on_step; edt и gs loop не дублируют список. Усилие: **M**.

### US-49-9 — Исправить нарушения слоёв
**Файлы:** `core/core/windows/window_service/CMakeLists.txt` (`gui_gl_backend` → PRIVATE-деталь, не PUBLIC),
`core/engine/scene/*` (убрать зависимость scene→gui: dependency inversion — gui сам берёт текстуру у renderer
через drv_interface/callback, scene/renderer её не знает).
**AC:** нижний `window` не зависит от верхнего `gui_gl_backend` публично; scene не включает gui-заголовки; сборка зелёная. Усилие: **M**.

---

## Граф зависимостей / порядок

```
EPIC-41 US-41-1/4/6 (ThirdParty, visibility, imgui)  ← фундамент, делать первым
  └── US-49-2 (структурные дефекты) ─┬─ US-49-3 (engine_api/core_api)
                                     └─ US-49-1 (public/private границы)
US-49-4 (тяжёлые заголовки)  ← независимо, дешёвые быстрые победы (entt/imgui fwd, res::tag)
US-49-7 (depends_on/топосорт) + US-49-8 (реестр стадий)  ← wiring, после/параллельно EPIC-29
US-49-9 (нарушения слоёв)
  └── US-49-6 (frame_context-инъекция, координ. EPIC-29 US-29-4)
        └── US-49-5 (POD-материалы + provider — главный шов, последним)
```

## Фазы

| Фаза | US | Результат |
|---|---|---|
| **0 — Фундамент (EPIC-41)** | 41-1/4/6 + US-49-2 | ThirdParty, visibility, imgui-таргет, common-lib, editor dup-fix |
| **1 — Дешёвые границы** | US-49-4 | entt/imgui fwd-decl, res::tag без json/glm — быстрый выигрыш компиляции |
| **2 — Контракты слоёв** | US-49-1, US-49-3 | public/private границы, engine_api/core_api |
| **3 — Wiring** | US-49-7, US-49-8 | depends_on+топосорт, реестр стадий кадра |
| **4 — Развязка scene↔render** | US-49-9, US-49-6, US-49-5 | нарушения слоёв, frame_context-инъекция, POD-материалы |

## Риски
- `PUBLIC→PRIVATE` вскроет неявные транзитивные зависимости — это ценная диагностика; менять по таргету,
  каждый шаг верифицировать `cmake --preset dev-debug && ctest` (307 тестов).
- US-49-5 (POD-материалы) — самый глубокий шов; делать последним, отдельным коммитом.
- Не смешивать переименования (EPIC-29 US-29-2) с visibility/boundary-правками — нечитаемый дифф.

## Критерии готовности
- [ ] Ни один таргет не открывает корень исходников как PUBLIC; у компонентов есть публичные API-границы
- [ ] `common` — самодостаточная STATIC-либа; editor_system не реконфигурирует core
- [ ] `engine_api`/`core_api` INTERFACE-контракты; агрегаторы PRIVATE
- [ ] Тяжёлые заголовки (res::tag/desc_base/entt/imgui) изолированы; время компиляции снижено
- [ ] scene не включает конкретные render-классы и не вызывает rnd::get_system(); scene не знает о gui
- [ ] Связывание модулей декларативно (depends_on+топосорт); per-frame стадии — через реестр
- [ ] `window` не зависит публично от `gui_gl_backend`
- [ ] Добавление нового модуля/компонента: новый таргет + 1-2 строки в агрегаторе/модуле, 0 правок в чужих файлах
- [ ] Все тесты проходят; поведение движка не изменилось
