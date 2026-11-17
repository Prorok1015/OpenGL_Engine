# Дизайн: физический public/ split заголовков (задача #109, эпик #11)

**Issue:** #109 (эпик #11 «EPIC-41 CMake hygiene») · **Основа:** ADR-03 (`docs/roadmap/CHIMERA_ROADMAP.md:125,137`), #108 (install/export, пакет `snake_sdk`) · **Статус:** реализовано (PR #214, #215, #228, #234, #235, #237); переходный `snk::scene_material_desc_api` удалён в #236: описание материала (`material_desc`, `pass_queue`) переехало из scene в render (`snk/render/rnd_material_desc.h`, namespace `rnd`), `snk/scene/scn_material_desc.h` оставляет псевдонимы `scn::material_desc` и `scn::pass_queue`, scene линкует render PUBLIC; render не включает заголовков `snk/scene/*` (решения — §6)

## 1. Как устроено сейчас

- **Поиск заголовков.** Каждый компонент делает корень своей папки include-каталогом: `target_include_directories(<tgt> PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})` (application, common, config, desc, ecs, resource, drv_opengl, window, game_system, gui_*, image, input, render, scene; `core_input`, `drv_interface`, `gui_backend_interface` — INTERFACE). Исходники и заголовки лежат вперемешку, `public`/`include` нет. Зонтики `core`/`engine` включают корень PRIVATE; публичный контракт слоя — INTERFACE-таргеты `snk::core_api` и `snk::engine_api` (EPIC-49).
- **Стиль include.** Всего ~1155 строк `#include "..."`, ведущих на заголовки `core/` (389 файлов); ~750 из них межкомпонентные. Плоские (`"res_system.h"`, `"common.h"`) — подавляющее большинство; с подпапкой относительно корня компонента — `"level/scn_level.h"`, `"ds/ds_store.hpp"`, `"logger/engine_log.h"`, `"resources/res_resource_text.h"`. Путей с `../` в include нет, коллизий базовых имён между заголовками нет (flat-namespace работает).
- **Обходы границ в CMake.** `render` добавляет PUBLIC `.../../scene` (ради `scn_material_desc.h`); `drv_opengl` добавляет PUBLIC `../../../../windows/window_service` (ради `wnd_gl_surface.h`); `Editor/CMakeLists.txt:17` добавляет `core/core` и `core/engine` ради `core_module.h`/`engine_module.h`; `window` держит `impl_glfw/` PRIVATE (единственное место, где уже есть физическое разделение).
- **Что ставит install** (`cmake/SnkInstall.cmake`, `_snk_sdk_prepare_includes`): для каждого include-каталога каждого таргета из замыкания (в статике — и PRIVATE-зависимости, т.е. `drv_opengl`) делается `install(DIRECTORY ... FILES_MATCHING *.h *.hpp *.inl ...)` в `include/snk/<путь от корня репо>/`. То есть **ставится всё**: и `impl_glfw/*`, и `rnd_gl_*.h`, и `passes/*`. Это прямо отмечено в шапке файла («public/ split later, #109»). Сгенерированных заголовков и export-header нет; `SNK_API` — рукописный `core/core/common/snk_api.h` (`SNK_ENGINE_SHARED`/`SNK_ENGINE_BUILD`).
- **Потребители.** Editor (`editor_system`) линкует `snk::engine` + `snk::engine_api`, тесты (`unit_tests`) — то же плюс `editor_system`, `tools/desc_resave`, `tests/package_consumer` (только `mem_allocator.h` и `res_tag.h`). Editor: 173 межкомпонентных include, тесты: 194, tools: 11.
- **Заголовков в `core/`:** 188 (Editor — 37, не экспортируется). По компонентам: scene 39, render_system 26, common 23 (+logger 1), video 18 (interface 9, opengl 9), resource 18, ecs 16, windows 11 (impl_glfw 4), gui 9, desc 8, application 7, input_system 6, core_input 2, image/game_system/config/модульные — по 1.
- **Форматы.** 299 из 456 исходников (.h/.hpp/.cpp) — CRLF, 157 — LF, `.gitattributes` нет (CLAUDE.md: табы). PCH (`cmake/pch/snk_pch*.h`) не включает ни одного проектного заголовка — split на него не влияет (`snk_pch_sees_all` смотрит на граф линковки сторонних библиотек).

## 2. Варианты

**(a) Физический переезд** в `<target>/include/snk/<component>/`, include переписываются на `<snk/<component>/x.h>`, внутренние заголовки остаются рядом с `.cpp` (или в `src/`).
- Плюсы: граница проверяется компилятором — внешний таргет физически не видит внутренний заголовок (`PUBLIC` = только `include/`); install тривиален (`install(DIRECTORY include/)`); устраняет flat-namespace; одинаковый путь в дереве и в SDK.
- Минусы: максимальный churn; затрагивает 100% потребителей; конфликты слияния с параллельными ветками.

**(b) Файлы на месте + список публичных** (`FILE_SET HEADERS` / явный список на таргет), ставится только список.
- Плюсы: ноль переименований, ноль правок include, blame цел.
- Минусы: **границу никто не проверяет при сборке** — внутренний заголовок по-прежнему виден соседям (корень остаётся PUBLIC include-dir), поэтому SDK может сломаться, когда публичный заголовок включит «внутренний»; ловится только в CI-job SDK. Плоские имена остаются в SDK (`#include "common.h"` у клиента — плохо). `FILE_SET` требует CMake ≥3.23 (сейчас `cmake_minimum_required(3.16)` в подпроектах; корень — проверить), и иерархия относительно `BASE_DIRS` всё равно складывается в корень компонента, а не в `snk/<component>/`.

**(c) Гибрид — рекомендуется.** Физический `include/snk/<component>/` (как (a)), но с пошаговым переездом по компонентам и без массового «одномоментного» rewrite: PR на компонент делает `git mv` публичных заголовков + переписывает все include **только на заголовки этого компонента**; установка в `SnkInstall.cmake` поддерживает оба раскладки на время миграции (непереехавший компонент ставится по-старому). `.cpp` не двигаем (меньше churn; `src/` — по желанию позже). Внутренние заголовки остаются в корне компонента, который становится PRIVATE include-dir.

**Оценка churn (git grep/скрипт по репо):**

| Показатель | (a)/(c) | (b) |
|---|---|---|
| Перемещаемых файлов | ~164 из 188 (24 остаются внутренними) | 0 |
| Строк `#include` к переписыванию | ~1103 в ~379 файлах (Editor 173, тесты 194, tools 11) + 52 строки на внутренние заголовки не меняются | 0 |
| Правок CMake | ~22 `CMakeLists.txt` + `SnkInstall.cmake` + `SnkTarget.cmake` (хелпер) | ~22 + install |
| Доки | ~369 упоминаний путей `core/core|core/engine` в docs/README/CLAUDE.md; правила `engine_log.h` в CLAUDE.md/CONSTRAINTS.toon | минимум |

- **MSVC shared / SNK_API:** путь заголовка не влияет на `__declspec`; опасно только одно — публичный заголовок, внутри которого ссылаются на заголовок из `src`, перестаёт собираться у потребителя DLL (поймает Windows-job `sdk-package-windows`). `snk_api.h` переезжает в `snk/common/snk_api.h`; INTERFACE-таргет `snk_api` (`core/core/CMakeLists.txt:39`) указывает на `common/include`. Определения `SNK_ENGINE_BUILD`/`IMGUI_API` в `snk_apply_target_defaults` завязаны на `SOURCE_DIR`, не на include-пути — не меняются.
- **PCH:** не затронут (нет проектных заголовков).
- **SDK-потребитель:** меняется `tests/package_consumer/main.cpp` (2 include) и раскладка `include/` в префиксе: `include/snk/<component>/...` вместо `include/snk/core/core/<dir>/...` — это ломающее изменение для кого-то, кто уже собрал против 0.1.0; поднять `SNK_SDK_VERSION` (сейчас 0.1.0, `SameMajorVersion`) → 0.2.0 или признать, что до 1.0 раскладка нестабильна.
- **CRLF:** `git mv` не меняет байты; скрипт переписывания include обязан работать с байтами (`open(..., 'rb')`, regex по `bytes`), не нормализуя EOL. Не вводить `.gitattributes` в этих же PR.
- **История/blame:** чистый `git mv` — rename 100%, `git log --follow`/`blame` проходят; правка include-строк оставляет файл с похожестью >95%. При squash-merge rename всё равно детектируется. После слияния — отдельный коммит в `.git-blame-ignore-revs` с SHA rewrite-коммитов.

**Рекомендация (c):** физический `include/snk/<component>/` по одному компоненту за PR, внутренние заголовки остаются в корне компонента (PRIVATE), `.cpp` не двигаем, CI-проверка «внутреннее не установлено» с первого PR.

## 3. Правило публичности и первая раскладка

**Правило.** Заголовок публичный, если он (1) включается из Editor, `tools/` или `tests/package_consumer`; или (2) включается из публичного заголовка (транзитивное замыкание); или (3) включается из другого компонента как часть его API. Внутренние: `impl_*`/`impl_glfw`, `backends/` конкретные реализации, `rnd_gl_*`, `passes/*`, менеджеры в `.cpp`-единицах. Запрет: публичный заголовок не включает glad/GLFW/assimp (сейчас так и есть — 8 заголовков с ними все внутренние). Тест-только: см. ниже (флаг).

**Имена каталогов** (по цели; решение владельца, §5): `snk/common` (+`ds/`, `logger/` как подпапки или отдельный `snk/logger`), `snk/config`, `snk/application`, `snk/core_input` (core/input) и `snk/input` (engine/input_system) — имена у `inp_`-префикса совпадают, поэтому разводим, `snk/window`, `snk/drv` (drv_interface), `snk/desc`, `snk/ecs`, `snk/resource`, `snk/render`, `snk/scene`, `snk/gui`, `snk/game`, `snk/image`.

| Компонент | Публичные (первая итерация) | Внутренние / спорные |
|---|---|---|
| common (23+1) | все `ds/*`, `mem_allocator.h`, `common.h`, `engine_log.h`, `engine_assert.h`, `eng_profiler.h`, `eng_transform_3d.hpp`, `timer.hpp`, `snk_api.h` | `mem_*_allocator.h`, `mem_debug_resource.h` — тест-только (спорно); `ds_rtree.hpp` — только внутри, `core_thread_utils.hpp` — только .cpp resource |
| application (7) | `app_frame_stage.h`, `app_loop_service*.h`, `app_module_interface.h` | `app_module_entry.h` (внутр.); `application.h`, `app_module_loader.hpp` — тест-только (спорно) |
| core_input (2) | оба | — |
| window (11) | `wnd_window.h`, `wnd_window_system.h`, `wnd_window_event_listener_interface.h`, `wnd_coordinates.h`, `wnd_gl_surface.h` (нужен `drv_opengl`) | `impl_glfw/*` (4) внутр.; `wnd_null_window_system.h` — тест-only (спорно); `wnd_window_service_init.h` — модульная проводка |
| drv_interface (9) | все `rnd_*_interface.h`, `rnd_buffer_layout.h` | `rnd_uniform_buffer_interface.h` — нигде не включён |
| drv_opengl (9) | нет | все `rnd_gl_*`, `open_gl_specific.h` внутр.; `rnd_gl_swapchain.h` и `rnd_gl_texture.h` включают .cpp `render` и `gui_gl_backend` — «дружественный» доступ (спорно) |
| desc (8) | `desc_system.h`, `desc_base.hpp`, `desc_guid.h`, `desc_node_id.h`, `desc_json_ops.h` | `desc_hints.hpp` — не используется; `adapters/desc_adapter_t.h` — tests/tools; `desc_desc_service_init.h` — проводка |
| ecs (16) | `ecs_system.h`, `ecs_entity.h`, `ecs_event.hpp`, `ecs_system_interface.hpp`, `ecs_system_plan.hpp`, `ecs_fixed_stepper.hpp`, `ecs_random.hpp`, `ecs_component.h`, `ecs_command_buffer.hpp`, `ecs_entity_changer.hpp` | `ecs_invoker/system_access/system_registry/traits` — только внутри (но тянутся публичными: проверить); `ecs_phase.hpp` — тесты |
| resource (18) | `res_system.h`, `res_tag.h`, `res_tag_json.h`, `resources/*` (4), `adapters/res_adapter_interface.hpp`, `res_pct_adapter.h` | `resolvers/*`, `res_text_adapter.h`, `res_adapter_worker*.h` — тесты/tools; `res_file_watcher.h`, `res_adapter_info.hpp` — внутр. |
| input_system (6) | `inp_ecs_input_manager.h`, `inp_events.hpp`, `inp_input_system.h`, `inp_input_manager_base.h` | `inp_input_consumable_listener.h`, `inp_input_service_init.h` — проводка |
| gui (9+2) | `gui_system.h`, `gui_layer_interface.h`, `gui_menu_layout.h`, `gui_backend_interface.h` | `gui_profiler_overlay.h` внутр.; `gui_debug_layer.h`, `gui_platform_backend.h` — тесты; `gui_gl_backend.h` — только .cpp gui_system |
| render (26) | `rnd_render_system.h`, `rnd_frame_assembler.h`, `rnd_frame_context.h`, `rnd_render_packet.hpp`, `rnd_light_data.hpp`, `rnd_extractor_interface.h`, `geom/rnd_geometry_desc.h`, `skinning/rnd_skinning_manager.h`, `shader/rnd_material_compiler.h` | `passes/*` (6), `rnd_gpu_profiler.h`, `texture/rnd_texture_{2d,cubemap}_desc.h` внутр.; **спорно:** `rnd_render_system.h` включает `texture/shader/geom manager` и `rnd_render_pass_interface.h` — они публичны транзитивно, пока не будет forward-decl/pimpl |
| scene (39) | `level/*` (9 из 10), `scn_*_desc.h`, `scn_camera*_component.hpp`, `scn_model.h`, `scn_mesh_nodes.hpp`, `scn_ecs_assembler.h`, `scn_registration_scope.h`, `scn_glm_json_convert.h`, `adapters/scn_model_importer_adapter.h` | `adapters/scn_assimp_resource_system_wrapper.h` внутр. (assimp); `level/scn_override_record.h` внутр.; tests-only: `scn_hot_reload_manager.h`, `scn_animation_job.h`, `scn_primitives.h`, `scn_transform_system.h`, `scn_camera_controller_system.h`, `scn_worldwrap_adapter.h` (спорно) |
| core_module.h / engine_module.h | в `snk/core/`, `snk/engine/` (Editor `main.cpp` их включает) — уберёт обход в `Editor/CMakeLists.txt:17` | — |

Итого первая итерация: ~140 публичных, ~24 внутренних, ~30 «тест-only» и «проводка» спорных (`*_service_init.h` — 8 штук, включает только `core_module.cpp`/`engine_module.cpp`). Умолчание: тест-only и проводка — публичные на первой итерации (минимальный риск), сужение отдельной задачей.

## 4. План по PR

0. **PR-0 инфраструктура (без переездов).** Хелпер `snk_component_headers(tgt [DIR include])` в `cmake/SnkTarget.cmake` (PUBLIC `$<BUILD_INTERFACE:include>`, PRIVATE корень); `SnkInstall.cmake`: `_snk_sdk_include_dest` для `*/include` → `include` (без префикса пути), старое поведение для непереехавших; скрипт CI `check-sdk-headers` (см. ниже); убрать обходы `../../scene` и `../../../../windows/window_service` (через линковку с публичным таргетом). Проверка: все job зелёные, SDK без изменений. Реализация: `render` линкует INTERFACE-таргет `snk::scene_material_desc_api` (каталог scene; прямая линковка `scene` дала бы цикл, переходный до PR-4), `drv_opengl` линкует `snk::window` (цикла нет: `window` не зависит от драйвера); хелпер подключён в `render` и `drv_opengl`.
1. **PR-1 пилот: `application`, `core_input`, `window`.** Малый fan-in, есть `impl_glfw`. Проверяет: install кладёт ровно `include/snk/window/*`, `impl_glfw` не в SDK, потребитель собирается. Здесь же: `tools/ci/check_sdk_headers.sh` (из PR-0, пока report-only) переключается на `ENFORCE_DEFAULT=1`, когда в SDK не остаётся запрещённых заголовков (последним PR-4), а проверка самодостаточности (TU с `#include <snk/...>` на каждый установленный заголовок в `package_consumer`, опция по умолчанию OFF) добавляется, когда первые компоненты переехали: сегодня она упала бы на внутренних заголовках.
2. **PR-2 ядро: `drv_interface`/`drv_opengl`, `ecs`, `desc`, `resource`.** Здесь меняется `package_consumer` (`<snk/resource/res_tag.h>`).
3. **PR-3 `common` (+`logger`, `config`)** — наибольший fan-in (`common.h`, `engine_log.h`, `snk_api.h` — сотни строк). Отдельным PR, чтобы откатываться независимо; перед ним — заморозить параллельные ветки. Проверяет Windows shared.
4. **PR-4 engine-слой: `input`, `image`, `game_system`, `gui(+backends)`, `render`, `scene`** (scene/render — самые крупные: ~65 файлов); `core_module.h`/`engine_module.h`.
5. **PR-5 финал:** убрать старую раскладку из `SnkInstall.cmake`, `SNK_SDK_VERSION` → 0.2.0, обновить CLAUDE.md/CONSTRAINTS.toon/ARCHITECTURE.toon/CODE_CONVENTIONS (`#include <snk/common/logger/engine_log.h>`), `PROJECT_STRUCTURE.md`, `.git-blame-ignore-revs`.

Каждый PR собирается: `git mv` + замена include по всем файлам компонента-потребителя (скрипт по байтам; ключ — резолв по базовому имени, коллизий нет) — состояние репо целиком консистентно, прокладок (shim) не нужно. Каждый PR доказывает: Linux gcc/clang static+shared, Windows MSVC shared (SDK job, `SNK_API`), `unit_tests`, headless smoke.

**CI-проверка (рекомендуется, в PR-0):** после `cmake --install` — (1) `find $PREFIX/include/snk` сверяется со списком запрещённых шаблонов (`impl_*`, `rnd_gl_*`, `passes/`, `glad`, `GLFW`) и с allow-списком на компонент; (2) `package_consumer` получает сгенерированную CMake’ом TU на каждый установленный `snk/**/*.h` (`#include <...>` по одному на файл), чтобы заголовки были самодостаточны и не ссылались на неустановленные; (3) `grep` по `package_consumer`/Editor/tests: запрет `#include "` на проектные заголовки (только `<snk/...>`). Источник правды границы — компилятор (внутренний каталог не в `PUBLIC`).

## 5. Риски и вопросы владельцу

1. **Именование каталогов** (`snk/core_input` vs `snk/input`, `snk/drv`, `logger` отдельно или в `common/`). Блокирует PR-1.
2. **Тесты и внутренности.** Тесты используют ~30 «не-API» заголовков (аллокаторы, resolvers, `scn_animation_job`). Оставить их публичными или дать `unit_tests` PRIVATE-доступ к `src/`-каталогам компонентов (больше правок CMake, строже API)?
3. **`*_service_init.h` и дружественные `rnd_gl_swapchain.h`/`rnd_gl_texture.h`:** публичные, или вынести в `<comp>/src` с явным PRIVATE-include у зонтиков/`render`/`gui`?
4. **Версия/совместимость SDK:** пакет 0.1.0 только что выпущен (#108) — повысить до 0.2.0 или считать раскладку нестабильной до 1.0?
5. **Окно заморозки:** переезд `common`/`scene` конфликтует с активными ветками — когда согласовать окно?
6. **Транзитивная публичность** `rnd_render_system.h` → менеджеры: чистить в рамках #109 или отдельной задачей (pimpl/forward-decl)?
7. **Риски:** (a) большой fan-in `common` — самый вероятный источник конфликтов; (b) `snk_sdk_closure` в статике экспортирует PRIVATE-зависимости — их `include/` тоже ставится, следует пустить `drv_opengl` без публичной части; (c) `FILE_SET` не используем (CMake 3.16 в подпроектах); (d) строки доков с путями (369) устареют — правка в PR-5.

## 6. Решения владельца (2026-10-18)

1. Каталоги: `snk/common` (логгер — `snk/common/logger`), `snk/core_input` (core/input) и `snk/input` (engine/input_system), `snk/driver` (drv_interface), остальные — как в §3.
2. Тест-only заголовки на первой итерации публичные; сужение — отдельной задачей.
3. `rnd_gl_*` (включая `rnd_gl_swapchain.h`, `rnd_gl_texture.h`) — внутренние, `render` и GUI-бэкенд получают явный PRIVATE-доступ; `*_service_init.h` пока публичные.
4. `SNK_SDK_VERSION` → 0.2.0 в PR-5.
5. PR-0 (инфраструктура, без переездов) — сразу; переезды (PR-1…) — после мержа #207, строго последовательно, без параллельных веток.
6. Транзитивная публичность менеджеров через `rnd_render_system.h` — отдельная задача, не в #109.
