# Handoff: состояние сборки / компонентизации (EPIC-49 done, EPIC-41 частично)

**Дата:** 2026-05-31
**Ветка:** `aibranch` (не дефолт; дефолт — `main`)
**TL;DR:** EPIC-49 (компонентизация + связывание) — ЗАВЕРШЁН (22 коммита, build + 312 тестов зелёные).
EPIC-41 (гигиена CMake) — частично: остались **US-41-7, US-41-8, US-41-10**. Плюс сознательно отложены
физический `public/` split и assembly-time инъекция skinning. **Runtime-долг:** рендер-пути не покрыты
unit-тестами — нужна ручная проверка запуском редактора.

---

## 1. Как СОБИРАТЬ и ПРОВЕРЯТЬ (важно — иначе верификация молча no-op)

⚠️ Документированный `cmd /c "...vcvars... && cmake"` ЛОМАЕТСЯ в Git Bash (мангл `/c` → cmd стартует
интерактивно и выходит, ничего не собрав; `./...exe` гоняет СТАРЫЙ бинарь). См. memory
`reference_windows_build.md`. Надёжный способ — `.bat`-хелпер + `cmd //c` (двойной слеш) с АБСОЛЮТНЫМ путём:

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAILED & exit /b 1)
cmake --preset dev-debug || (echo CONFIGURE_FAILED & exit /b 1)
cmake --build build/dev-debug --target unit_tests editor || (echo BUILD_FAILED & exit /b 1)
echo === BUILD_OK ===
```
Запуск: `cmd //c "C:\...\build_verify.bat"` (хелпер не коммитим). Оракул:
`./build/dev-debug/unit_tests.exe` → `*** No errors detected`, **312** кейсов, exit 0.
cmake 4.3.0 уже в PATH; MSVC-окружение — только после vcvars. Пресет `dev-debug` → `build/dev-debug/`.
(Есть ещё старый `build/` в корне — НЕ preset-based; предпочитать preset.)

---

## 2. Текущая CMake-структура (что построено в EPIC-49)

```
CMakeLists.txt (root)
├── C++20; ENGINE_PROFILE_MODE; THIRD_PARTY_PATH / ENGINE_CORE_PATH / SE_PROJECT_SOURCE_PATH
├── include(ThirdParty)              # cmake/ThirdParty.cmake (через CMAKE_MODULE_PATH)
├── add_subdirectory(core)           # ← движок вносится ОТСЮДА (было: из editor_system)
├── add_subdirectory(Editor)
└── unit_tests → snk::editor_system + snk::engine + snk::engine_api + snk::config_system

cmake/
├── ThirdParty.cmake   # ВСЕ third-party cache-vars (GLFW_*/ASSIMP_*/SPDLOG_*) + ЕДИНЫЙ BUILD_SHARED_LIBS
│                       # + Boost FetchContent slim: BOOST_INCLUDE_LIBRARIES=json;asio;program_options;test
│                       # + include(imgui)
└── imgui.cmake        # таргет imgui / alias imgui::imgui (НЕ внутри сабмодуля lib3dparty/imgui)

Слои (все движковые таргеты имеют snk:: alias; зависимости через snk::):
LAYER 1 COMMON  snk::common (STATIC, вобрал mem_*.cpp)   snk::engine_logger (STATIC)
LAYER 2 CORE    snk::ecs / resource_system / desc_system / window / application /
                config_system / drv_opengl / core_input(INTERFACE) / drv_interface(INTERFACE)
                ├── snk::core      (STATIC, зависимости PRIVATE, include корня PRIVATE)
                └── snk::core_api  (INTERFACE) = common/application/ecs/desc_system/resource_system/
                                    core_input/drv_interface/window/config_system  (БЕЗ drv_opengl)
LAYER 3 ENGINE  snk::render / scene / gui / gui_gl_backend / input_system / game / image /
                gui_backend_interface(INTERFACE)
                ├── snk::engine     (STATIC, зависимости PRIVATE, include корня PRIVATE; линкует snk::core_api для своих .cpp)
                └── snk::engine_api (INTERFACE) = snk::core_api + scene/render/gui/input_system/game  (БЕЗ gui_gl_backend)
LAYER 4 EDITOR  snk::editor_system → PRIVATE snk::engine (символы) + PRIVATE snk::engine_api (заголовки) + assimp
                editor (exe) → editor_system + engine + engine_api + config_system
                              + PRIVATE include ${ENGINE_CORE_PATH}/core и /engine (для core_module.h/engine_module.h)
```

**Достигнутые инварианты:**
- Зонтики core/engine PRIVATE — не «светят всё всем»; кросс-слой идёт через `*_api` INTERFACE.
- Потребитель линкует ПАРУ: `snk::engine` (объектный код) + `snk::engine_api` (заголовки). api НЕ линкует umbrella (иначе ре-leak).
- Тяжёлые заголовки изолированы: entt/fwd.hpp в scene-хедерах; imgui.h убран из публичных хедеров (реплика integer-typedef); `res_tag.h` без boost/json+glm (вынесено в `res_tag_json.h`); `desc_base.hpp`→`boost/json/fwd.hpp`.
- Связывание модулей декларативно: `module_interface::depends_on()` + один топосорт в `app_module_loader.hpp` (register==initialize==топо; shutdown=reverse). core→engine→editor.
- Кадровый цикл: `gs::frame_stage_registry` + `frame_step_context`; обе петли (edt/gs) строят реестр и зовут `run(ctx)` (order-ключи 10/20/[25]/28/30/40/[45]/50). Стадия 28 = `render_system::begin_frame` (инъекция менеджеров).
- scene↔render развязаны: `scn::material_desc` — POD (без rnd_*); компиляция в render-side `rnd::material_compiler_interface`; менеджеры из `frame_context` (нет `rnd::get_system()` в scene/пассах); window не зависит от gui (gui сам владеет imgui-бэкендом).

---

## 3. ФРОНТ РАБОТ (с чего начать)

### A. Закрыть EPIC-41 (гигиена CMake) — `docs/epics/infrastructure/EPIC-41.md` · backlog **BL-008** (`docs/agent/backlog/items/BL-008.toon`)
- **US-41-10 (low-risk, проверяется сборкой):** создать `cmake/CompilerWarnings.cmake` с INTERFACE-таргетом
  `compiler_warnings` (MSVC `/W3`; GCC/Clang `-Wall -Wextra -Wpedantic -Wno-unused-parameter`), подключить к
  `common` через `target_link_libraries(common INTERFACE compiler_warnings)`. Third-party не должны получать флаги.
- **US-41-8 (low-risk):** убрать хардкод путей. `SE_RESOURCE_ABSOLUTE_PATH` задаётся в `Editor/CMakeLists.txt:3`,
  используется в `core/core/resource/CMakeLists.txt` (RESOURCE_PATH define) — неявная зависимость порядка
  конфигурации. `THIRD_PARTY_PATH` — глобальная (заменить на `${CMAKE_SOURCE_DIR}/lib3dparty` или одноразовый set).
  `SE_PROJECT_SOURCE_PATH`→`PROJECT_SOURCE_PATH` compile-def в `core/core/video/drivers/impls/opengl/CMakeLists.txt:4`.
- **US-41-7 (координировать с EPIC-34):** `assimp` линкуется напрямую в `core/engine/scene/CMakeLists.txt:14`
  (`#temporary`), `core/core/resource/CMakeLists.txt`, `Editor/code/editor_system/CMakeLists.txt`. Цель — вынести
  assimp-зависимый код в опциональный модуль `assimp_importer` (EPIC-34 US-34-8). Не делать в отрыве от EPIC-34.

### B. Отложенные части EPIC-49 (зафиксированы в EPIC-49.md)
- **Физический `public/` split (US-49-1, этап 2):** сейчас компонентные dirs всё ещё открывают свой корень
  PUBLIC внутри слоя; границы держатся на `*_api` INTERFACE. Полный разнос заголовков на `public/`+`private/`
  отложен (по эпику — допустимо). Делать ПОСЛЕ исправления visibility (уже сделано), отдельно, НЕ мешая с ренеймами (EPIC-29 US-29-2).
- **Assembly-time skinning инъекция** (backlog **BL-007** — `docs/agent/backlog/items/BL-007.toon`): `core/engine/scene/scn_skin_weights_desc.cpp:52` зовёт
  `rnd::get_system().get_skinning_manager()` на ECS-assembly-time (`gs_game_init.cpp:142`), НЕ per-frame — поэтому
  исключён из US-49-6 (там нет frame_context). Нужно протащить `skinning_manager&`/коллбэк через ассемблер
  (координировать с EPIC-29 US-29-4 / EPIC-33).
- **Латентное:** `scn_material_desc::serialize` больше не фильтрует шейдер-теги по расширению (строго корректнее;
  отличие только для нестандартных расширений). Не баг.

### C. ⚠️ RUNTIME-ВЕРИФИКАЦИЯ (обязательно перед мержем в main)
Фазы 3–4 меняли кадровый цикл и рендер-путь, которые **НЕ покрыты** 312 unit-тестами. Build зелёный ≠ рендер
работает. Запустить редактор и проверить глазами:
1. Сцена рендерится (опак-меши, текстуры, цвета материалов — albedo/emissive).
2. Skybox виден. 3. Анимированная/скелетная модель деформируется (USE_ANIMATION). 4. Debug-кости (`show_skeleton`).
5. Направленный свет влияет на затенение (DIRECTION_LIGHT_COUNT). 6. MIX-материал в opaque И transparent.
7. Мультимир (переключение в иерархии). 8. Resize вьюпорта (регенерация RT). 9. Save level → load → diff JSON (round-trip).

---

## 4. Карта ключевых файлов (свежесозданное в EPIC-49)
- `cmake/ThirdParty.cmake`, `cmake/imgui.cmake`
- `core/core/resource/res_tag_json.h` (json/tag_invoke вынесены из res_tag.h)
- `core/engine/game_system/gs_frame_stage.{h,cpp}` (frame_stage_registry + register_core_frame_stages)
- `core/engine/render/render_system/shader/rnd_material_compiler.{h,cpp}` (material_compiler_interface + default_material_compiler)
- `tests/scn_material_desc_tests.cpp` (round-trip + render_mode таблица)
- INTERFACE-таргеты `core_api`/`engine_api` — в `core/core/CMakeLists.txt` / `core/engine/CMakeLists.txt`.

## 5. Ссылки
- Эпики: `docs/epics/infrastructure/EPIC-49.md` (все US с пометками DONE + коммиты), `EPIC-41.md` (прогресс/остаток), `EPIC-29.md` (координация Service Layer).
- Архив воркпланов: `.claude/workplan/archive/2026-05-31-epic49-phase{0,1,2,3,4}.md` (детальный discovery+plan каждой фазы).
- Исходный бриф: `docs/agent/handoff/EPIC-49-agent-brief.md`.
- Backlog follow-ups: **BL-007** (`docs/agent/backlog/items/BL-007.toon`, assembly-time skinning), **BL-008** (`docs/agent/backlog/items/BL-008.toon`, остаток EPIC-41) — индекс `docs/agent/backlog/INDEX.toon`.
- Memory: `reference_windows_build.md` (build-гоча), `reference_frame_allocator.md`, `reference_ecs.md`.
- Коммиты EPIC-49: `1441bd6`(49-2) `ca6aef9`(41-1) `cedc10b`(41-6) `2d89688`+`2714c1e`(41-4) `4e9688d`+`484e492`+`092b315`+`d44b492`(49-4) `fa142b5`+`d489a98`(49-3/1) `cf82fd9`(49-7) `64f7a0b`(49-8) `3cf68bc`+`0dff7ca`(49-9) `bcf7f49`(49-6) `3ed2cc9`(49-5) + docs.

## 6. Правила работы (из CLAUDE.md / брифа)
DISCOVER→PLAN→CONFIRM(жёсткий гейт)→IMPLEMENT→VERIFY→REPORT, воркплан в `.claude/workplan/`. Behavior-preserving,
верификация сборкой+тестами на каждом шаге; рискованные швы — адверсариальная проверка отдельным агентом.
Коммитить точечно по путям (НЕ `git add -A`; не трогать сдвинутые сабмодули, `*.desc`, `claude-framework/`).
Конец git-сообщения: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
