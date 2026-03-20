# EPIC-41: CMake Build System — продакшн-качество сборки

**Status:** planned
**Theme:** infrastructure
**Dependencies:** none (standalone, но US-41-7 координируется с EPIC-34 US-34-8)

---

## Мотивация

Текущая система сборки исторически складывалась без единого стандарта. Она работает, но содержит ряд проблем, которые затрудняют поддержку, замедляют сборку и блокируют будущие фичи (опциональные модули, кросс-платформа, CI).

### Выявленные проблемы

**1. Глобальное загрязнение (global pollution)**
- `BUILD_SHARED_LIBS OFF` выставляется внутри `resource_system/CMakeLists.txt` — влияет на **все** таргеты проекта.
- `CMAKE_CXX_STANDARD 20` дублируется 3 раза (root, `core/`, `common/`).
- Cache-переменные третьесторонних библиотек (`GLFW_BUILD_DOCS`, `ASSIMP_BUILD_TESTS`, `ASSIMP_INSTALL` и т.д.) разбросаны по `window/`, `resource_system/` вместо того чтобы быть в одном месте.

**2. Отсутствие/неконсистентность visibility specifiers**
- `application/CMakeLists.txt`: `target_link_libraries(... common)` и `target_link_libraries(... Boost::json)` — **без** `PRIVATE/PUBLIC`, CMake считает PUBLIC, утечка зависимостей.
- Часть таргетов без явного `STATIC`: `application`, `gui`, `render`, `input_system`, `drv_opengl`, `gui_gl_backend` — тип определяется `BUILD_SHARED_LIBS`.
- `editor_system → engine` линкуется как `PUBLIC`, хотя должен быть `PRIVATE`.

**3. GLOB_RECURSE без CONFIGURE_DEPENDS**
- 11+ мест используют `file(GLOB_RECURSE ...)` без `CONFIGURE_DEPENDS`. Добавление нового `.cpp` файла не триггерит перегенерацию.
- Переменные-коллизии: `SE_RENDER_SOURCES` используется одновременно в `ecs/`, `render/`, `scene/` (разные скоупы, но путает разработчика).

**4. ImGui встроен напрямую в gui_gl_backend**
- 7 `.cpp` файлов ImGui компилируются как `target_sources(gui_gl_backend ...)` с хардкод-путями в `lib3dparty/imgui/`.
- Нет отдельного таргета `imgui` — невозможно переиспользовать или подменить.

**5. Хардкод путей и неявные зависимости**
- `SE_RESOURCE_ABSOLUTE_PATH` задаётся в `Editor/CMakeLists.txt`, но используется как `target_compile_definitions` в `resource_system/`. Работает только потому что Editor конфигурируется первым.
- `SE_PROJECT_SOURCE_PATH` передаётся в `drv_opengl` для неизвестных целей.
- `THIRD_PARTY_PATH` — глобальная переменная, используемая в дочерних CMake без гарантии что она определена.

**6. Assimp линкуется в 3 местах**
- `scene/` — `PRIVATE assimp` (с комментарием `#temporary`)
- `editor_system/` — `PRIVATE assimp`
- `resource_system/` — `PRIVATE assimp` (для адаптеров)
- Решается в EPIC-34 US-34-8, но нужна подготовка на уровне CMake.

**7. Нет install-таргетов и экспорта**
- Невозможно `find_package(OpenGLEngine)` из внешнего проекта.
- Нет `install(TARGETS ...)` ни для одного таргета.
- Нет `CMakePresets.json` для стандартизации конфигурации.

---

## Архитектурное решение

### Целевая структура CMake

```
CMakeLists.txt (root)
  ├── cmake/                          ← helper-модули
  │   ├── ThirdParty.cmake            ← ВСЕ настройки третьесторонних зависимостей
  │   └── CompilerWarnings.cmake      ← общие флаги компиляции
  ├── CMakePresets.json               ← presets: debug, release, ci
  ├── lib3dparty/                     ← git submodules (без изменений)
  ├── core/
  │   ├── core/                       ← STATIC libraries (без изменений)
  │   └── engine/                     ← STATIC libraries (без изменений)
  ├── Editor/                         ← executable
  └── unittests/                      ← executable
```

### Принципы

1. **Один источник правды для C++ стандарта** — только в root `CMakeLists.txt`.
2. **Все третьесторонние cache-переменные** — в `cmake/ThirdParty.cmake`, до `add_subdirectory`.
3. **Явный `STATIC` на всех библиотеках** — никаких авто-определений.
4. **Visibility на каждом `target_link_libraries`** — PRIVATE по умолчанию, PUBLIC только для API.
5. **CONFIGURE_DEPENDS на всех GLOB** — или явное перечисление файлов.
6. **ImGui как отдельный STATIC таргет** — переиспользуемый, подменяемый.
7. **CMakePresets.json** — стандартный способ конфигурации для разработчиков и CI.
8. **Namespace-алиасы `snk::`** — все движковые таргеты оборачиваются в `add_library(snk::X ALIAS X)`. Название проекта: **Snake Engine**.

---

## User Stories

### US-41-1: Централизовать настройки третьесторонних библиотек
**Файлы:** `cmake/ThirdParty.cmake` (новый), `CMakeLists.txt` (root), `core/core/windows/window_service/CMakeLists.txt`, `core/core/resource/CMakeLists.txt`

Собрать все cache-переменные третьесторонних библиотек в один файл `cmake/ThirdParty.cmake`. Удалить их из дочерних CMakeLists.

**AC:**
- [ ] Создан `cmake/ThirdParty.cmake` со всеми `set(... CACHE ...)` для glfw, assimp, yaml-cpp, spdlog и др.
- [ ] Root CMakeLists.txt делает `include(cmake/ThirdParty.cmake)` перед первым `add_subdirectory`
- [ ] `BUILD_SHARED_LIBS OFF` задаётся **только** в `ThirdParty.cmake`, удалён из `resource_system/CMakeLists.txt`
- [ ] `GLFW_BUILD_DOCS/TESTS/EXAMPLES OFF` удалены из `window/CMakeLists.txt`
- [ ] `ASSIMP_BUILD_TESTS/ASSIMP_INSTALL OFF` удалены из `resource_system/CMakeLists.txt`
- [ ] FetchContent для Boost перемещён в `ThirdParty.cmake`
- [ ] Проект собирается и все тесты проходят

### US-41-2: Убрать дублирование CMAKE_CXX_STANDARD
**Файлы:** `CMakeLists.txt` (root), `core/CMakeLists.txt`, `core/core/common/CMakeLists.txt`

`CMAKE_CXX_STANDARD 20` задаётся трижды. Оставить только в root.

**AC:**
- [ ] `set(CMAKE_CXX_STANDARD 20)` присутствует **только** в root `CMakeLists.txt`
- [ ] Удалён из `core/CMakeLists.txt` и `core/core/common/CMakeLists.txt`
- [ ] Проект собирается без ошибок

### US-41-3: Явный STATIC на всех библиотечных таргетах
**Файлы:** `core/core/application/CMakeLists.txt`, `core/engine/render/render_system/CMakeLists.txt`, `core/engine/input/CMakeLists.txt`, `core/engine/gui/gui_system/CMakeLists.txt`, `core/engine/gui/backends/opengl/CMakeLists.txt`, `core/core/video/drivers/impls/opengl/CMakeLists.txt`

Таргеты без явного типа (`add_library(name)`) сейчас зависят от `BUILD_SHARED_LIBS`. Нужно явно указать `STATIC`.

**AC:**
- [ ] Все `add_library(name)` заменены на `add_library(name STATIC)` для: `application`, `render`, `input_system`, `gui`, `gui_gl_backend`, `drv_opengl`
- [ ] Таргеты `common`, `core_input`, `drv_interface`, `gui_backend_interface` остаются `INTERFACE` (корректно)
- [ ] Проект собирается без изменений в поведении

### US-41-4: Исправить visibility specifiers
**Файлы:** `core/core/application/CMakeLists.txt`, `Editor/code/editor_system/CMakeLists.txt`, `core/engine/game_system/CMakeLists.txt`, `core/core/ecs/CMakeLists.txt`

Привести все `target_link_libraries` к явному `PRIVATE`/`PUBLIC` с обоснованием.

**AC:**
- [ ] `application/CMakeLists.txt`: `common` → `PRIVATE common`, `Boost::json` → `PUBLIC Boost::json` (нужен для хедеров `app_data_storage`)
- [ ] `editor_system/CMakeLists.txt`: `PUBLIC engine` → `PRIVATE engine` (editor_system — конечный потребитель, не API)
- [ ] `ecs/CMakeLists.txt`: `PRIVATE common` — подтвердить что `common` не нужен в PUBLIC-хедерах ecs (иначе `PUBLIC`)
- [ ] Проверить все остальные таргеты — каждый `target_link_libraries` имеет явный visibility
- [ ] Все движковые таргеты обёрнуты в `add_library(snk::X ALIAS X)`: `snk::common`, `snk::application`, `snk::ecs`, `snk::scene`, `snk::render`, `snk::gui`, `snk::gui_gl_backend`, `snk::input_system`, `snk::engine`, `snk::core` и т.д.
- [ ] Зависимости между движковыми таргетами используют `snk::` алиасы (например `target_link_libraries(engine PRIVATE snk::scene snk::ecs)`)
- [ ] Собрать полный список изменений visibility перед применением
- [ ] Проект собирается и линкуется корректно

### US-41-5: Добавить CONFIGURE_DEPENDS ко всем GLOB
**Файлы:** все CMakeLists.txt с `file(GLOB ...)` или `file(GLOB_RECURSE ...)`

Добавить `CONFIGURE_DEPENDS` ко всем glob-вызовам, чтобы CMake отслеживал новые файлы. Исправить коллизии имён переменных.

**AC:**
- [ ] Все `file(GLOB ...)` и `file(GLOB_RECURSE ...)` содержат `CONFIGURE_DEPENDS`
- [ ] Переменные переименованы для уникальности: `SE_RENDER_SOURCES` в `ecs/` → `SE_ECS_SOURCES`, в `scene/` → `SE_SCENE_SOURCES`, и т.д.
- [ ] Удалён неиспользуемый `file(GLOB SE_SOURCES_H ...)` из `core/core/CMakeLists.txt`
- [ ] Проект корректно подхватывает новые `.cpp` файлы без ручного `cmake --build` reconfigure

### US-41-6: Выделить ImGui в отдельный STATIC таргет
**Файлы:** `lib3dparty/imgui/CMakeLists.txt` (новый), `core/engine/gui/backends/opengl/CMakeLists.txt`, `core/engine/gui/backends/interface/CMakeLists.txt`

Сейчас 7 файлов ImGui компилируются напрямую в `gui_gl_backend` через хардкод-пути. Создать отдельный таргет.

**AC:**
- [ ] Создан `lib3dparty/imgui/CMakeLists.txt` с таргетом `imgui` (STATIC)
- [ ] `imgui` таргет компилирует: `imgui.cpp`, `imgui_demo.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`
- [ ] `imgui` таргет предоставляет `PUBLIC` include directory для хедеров ImGui
- [ ] Backend-файлы (`imgui_impl_glfw.cpp`, `imgui_impl_opengl3.cpp`) остаются в `gui_gl_backend` (они зависят от glfw/glad)
- [ ] `gui_backend_interface` линкует `PUBLIC imgui` вместо `target_include_directories(... ${THIRD_PARTY_PATH}/imgui)`
- [ ] `gui_gl_backend` линкует `PRIVATE imgui` и убирает хардкод `target_sources` для imgui core файлов
- [ ] Проект собирается и ImGui работает как раньше

### US-41-7: Убрать прямую зависимость scene и resource_system от Assimp
**Файлы:** `core/engine/scene/CMakeLists.txt`, `core/core/resource/CMakeLists.txt`
**Зависимости:** координируется с EPIC-34 US-34-8 (опциональный `assimp_importer` модуль)

Подготовительный шаг: убрать `PRIVATE assimp` из `scene/` и `resource_system/`. Assimp-зависимый код (адаптеры) будет перемещён в `assimp_importer/` в рамках US-34-8.

**AC:**
- [ ] `scene/CMakeLists.txt` не содержит `target_link_libraries(... assimp)` — строка с `#temporary` удалена
- [ ] `resource_system/CMakeLists.txt` не содержит `target_link_libraries(... assimp)` (если есть assimp-зависимый код в resource — перенести)
- [ ] Assimp-зависимые `.cpp/.h` файлы из `scene/adapters/` помечены или перемещены
- [ ] Проект собирается (адаптеры временно могут быть отключены до завершения US-34-8)

### US-41-8: Убрать хардкод путей и неявные зависимости
**Файлы:** `Editor/CMakeLists.txt`, `core/core/resource/CMakeLists.txt`, `core/core/video/drivers/impls/opengl/CMakeLists.txt`

`SE_RESOURCE_ABSOLUTE_PATH` задаётся в Editor/, используется в resource_system/ — неявная зависимость порядка конфигурации. `SE_PROJECT_SOURCE_PATH` передаётся в drv_opengl.

**AC:**
- [ ] `SE_RESOURCE_ABSOLUTE_PATH` определяется **рядом** с `resource_system` или передаётся явно через `target_compile_definitions` в точке использования, не через глобальную переменную из Editor/
- [ ] `THIRD_PARTY_PATH` заменён на `${CMAKE_SOURCE_DIR}/lib3dparty` в местах использования, или вынесен в root как одноразовый `set()` перед всеми `add_subdirectory`
- [ ] `SE_PROJECT_SOURCE_PATH` — выяснить назначение; если для шейдеров — заменить на runtime-конфигурацию или `target_compile_definitions` в правильном месте
- [ ] Порядок `add_subdirectory` не влияет на корректность сборки (нет неявных зависимостей через переменные)

### US-41-9: Добавить CMakePresets.json
**Файлы:** `CMakePresets.json` (новый), `.gitignore` (обновить при необходимости)

Стандартный файл пресетов для разработчиков и будущего CI.

**AC:**
- [ ] Создан `CMakePresets.json` с пресетами:
  - `dev-debug` — Debug build, `ENABLE_ASSIMP_IMPORTER=ON`, `ENGINE_PROFILE_MODE=INTERNAL`
  - `dev-release` — RelWithDebInfo build
  - `release` — Release build, без профайлера
  - `no-assimp` — Debug build, `ENABLE_ASSIMP_IMPORTER=OFF` (для тестирования сборки без Assimp)
- [ ] Build presets: `build-debug`, `build-release`
- [ ] `cmake --preset dev-debug && cmake --build --preset build-debug` работает из корня проекта
- [ ] `CMakeUserPresets.json` добавлен в `.gitignore` (для локальных оверрайдов)

### US-41-10: Добавить compiler warnings
**Файлы:** `cmake/CompilerWarnings.cmake` (новый), `CMakeLists.txt` (root)

Стандартный набор предупреждений компилятора для раннего обнаружения багов.

**AC:**
- [ ] Создан `cmake/CompilerWarnings.cmake` с INTERFACE-таргетом `compiler_warnings`
- [ ] Для GCC/Clang: `-Wall -Wextra -Wpedantic -Wno-unused-parameter`
- [ ] Для MSVC: `/W3` (не `/W4` — слишком шумно для третьесторонних хедеров)
- [ ] `compiler_warnings` подключается к `common` через `target_link_libraries(common INTERFACE compiler_warnings)` — распространяется на весь проект
- [ ] Третьесторонние библиотеки **не** получают эти флаги (линкуются до `compiler_warnings` или через `SYSTEM` include)
- [ ] Проект собирается без новых ошибок (warnings допустимы на первом этапе, критичные — исправить)

---

## Порядок выполнения

```
US-41-2 (убрать дубли CXX_STANDARD) ─┐
US-41-3 (явный STATIC)               ├── параллельно, независимы
US-41-5 (CONFIGURE_DEPENDS)          ─┘
  │
  ▼
US-41-1 (централизация ThirdParty.cmake)
  │
  ├── US-41-4 (visibility specifiers) — после US-41-1, т.к. может быть конфликт
  ├── US-41-6 (ImGui таргет) — после US-41-1, т.к. ImGui настройки в ThirdParty
  ├── US-41-8 (хардкод путей) — после US-41-1
  └── US-41-7 (убрать assimp из scene/resource) — координация с EPIC-34 US-34-8

US-41-9 (CMakePresets) — параллельно с любой задачей
US-41-10 (compiler warnings) — после US-41-1
```

## Риски

- **Visibility changes могут сломать линковку.** Переход `PUBLIC → PRIVATE` может скрыть транзитивные зависимости, на которые неявно полагались другие таргеты. Нужно менять по одному таргету и проверять сборку.
- **CONFIGURE_DEPENDS — не серебряная пуля.** CMake документация предупреждает что `CONFIGURE_DEPENDS` может быть медленным на некоторых генераторах. На практике для проекта такого размера это не проблема, но стоит мониторить время конфигурации.
- **BUILD_SHARED_LIBS=OFF в ThirdParty.cmake.** Перенос из resource_system/ в root может повлиять на таргеты, которые раньше конфигурировались до этого `set()`. Проверить порядок.
- **ImGui как отдельный таргет.** ImGui backends (`imgui_impl_glfw.cpp`, `imgui_impl_opengl3.cpp`) зависят от glfw/glad — их нельзя включить в таргет `imgui`. Нужно разделить core ImGui и backends.
- **SE_RESOURCE_ABSOLUTE_PATH.** Используется для `RESOURCE_PATH` compile definition. Нужно понять как runtime резолвит ресурсы — возможно переход на cfg-файл предпочтительнее хардкода в compile time.

## Критерии завершения эпика

- [ ] Все настройки третьесторонних библиотек — в `cmake/ThirdParty.cmake`
- [ ] `CMAKE_CXX_STANDARD` задан один раз в root
- [ ] Все `add_library` имеют явный тип (`STATIC`/`INTERFACE`)
- [ ] Все `target_link_libraries` имеют явный visibility (`PRIVATE`/`PUBLIC`/`INTERFACE`)
- [ ] Все `file(GLOB...)` используют `CONFIGURE_DEPENDS`
- [ ] ImGui — отдельный CMake-таргет
- [ ] Нет прямой зависимости `scene` и `resource_system` от Assimp
- [ ] `CMakePresets.json` работает для debug/release/no-assimp конфигураций
- [ ] Compiler warnings включены для проектного кода, отключены для third-party
- [ ] Все движковые таргеты имеют `snk::` алиасы и зависимости используют алиасы
- [ ] Нет глобального загрязнения — `BUILD_SHARED_LIBS`, пути и cache-переменные определены в правильных местах
