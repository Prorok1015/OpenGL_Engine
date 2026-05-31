# Бриф для агента-исполнителя: EPIC-49 «Engine Componentization & Module Wiring»

> Это самодостаточный промпт для передачи разработки. Скопируй его целиком в новую сессию агента.
> Агент стартует «с холода» — здесь есть всё, что нужно, плюс ссылки на файлы для деталей.

---

## РОЛЬ И ЦЕЛЬ

Ты — инженер C++20 OpenGL движка с редактором по пути
`C:\Users\proro\source\repos\Prorok1015\OpenGL_Engine`.
Твоя задача — РЕАЛИЗОВАТЬ **EPIC-49: Engine Componentization & Module Wiring**: переработать сборку и
связывание модулей так, чтобы движок был разбит на чистые компоненты с настоящими границами, до того
как активный рост числа модулей начнёт «стрелять в ногу». Работа behavior-preserving (поведение движка
не меняется), верифицируется сборкой + тестами на каждом шаге.

## ЧТО ПРОЧИТАТЬ ПЕРВЫМ (обязательно)
1. `docs/epics/infrastructure/EPIC-49.md` — твой эпик: мотивация, целевая модель (4 слоя), 9 user stories
   (US-49-1..9), граф зависимостей, фазы миграции, риски, критерии готовности. ЭТО ГЛАВНЫЙ ДОКУМЕНТ.
2. `docs/epics/infrastructure/EPIC-41.md` — CMake-гигиена. Срез US-41-2/3/5/9 уже СДЕЛАН (single CXX_STANDARD,
   явный STATIC, CONFIGURE_DEPENDS, CMakePresets.json). Осталось US-41-1/4/6/7/8/10 — EPIC-49 Фаза 0 опирается
   на US-41-1/4/6. Координируйся, не дублируй.
3. `docs/epics/infrastructure/EPIC-29.md` — Service Layer (удалить game_system, убрать globals/get_system,
   inline construct/destruct). US-49-6/7/8 координируются с EPIC-29. Не дублируй.
4. `CLAUDE.md` (корневой) и `Editor/CLAUDE.md` — правила проекта. И `docs/agent/`:
   ARCHITECTURE.toon, CODE_CONVENTIONS.toon, CONSTRAINTS.toon, QUICK_LINKS.toon.
5. Воркфлоу-правила: `docs/agent/workflow/` (PHASES, REFACTOR, GATES, STATE_TRACKING).
6. Делегирование: `docs/agent/delegation/` (TRIGGERS, ROUTING, PATTERNS).

## МАНДАТОРНЫЙ ВОРКФЛОУ (строго)
Для КАЖДОЙ user story (это рефакторинг): **DISCOVER → PLAN → CONFIRM → IMPLEMENT → VERIFY → REPORT**.
- **DISCOVER:** прочитай реальный код, найди все точки правок, существующие паттерны. Делегируй широкое
  чтение суб-агентам (см. ниже).
- **PLAN:** конкретный план (файлы to change/add/delete, тесты/верификация, риски, out-of-scope, rollback).
- **CONFIRM (ЖЁСТКИЙ ГЕЙТ):** покажи план пользователю и дождись ЯВНОГО «да/proceed». «Звучит ок» / «наверное»
  — НЕ одобрение. Без одобренного плана НЕ пиши production-код.
- **IMPLEMENT:** строго по плану, без scope-drift. При отклонении (нашёл констрейнт) — СТОП, вернись к PLAN.
- **VERIFY:** собери и прогони тесты (см. команды). Не объявляй успех без доказательства.
- **REPORT:** кратко что изменилось, статус верификации, что осталось.

State tracking: веди `.claude/workplan/<slug>.md` (шаблон в `docs/agent/workflow/STATE_TRACKING.toon`),
архивируй в `.claude/workplan/archive/` по завершении. `.claude/` в .gitignore — workplan не коммитится.

## АКТИВНОЕ ИСПОЛЬЗОВАНИЕ СУБ-АГЕНТОВ (требование заказчика)
Делегируй агрессивно — это экономит контекст и ускоряет. Конкретно:
- **DISCOVER-фан-аут:** когда нужно прочитать много CMakeLists/заголовков (а в этом эпике — нужно),
  запускай НЕСКОЛЬКО агентов ПАРАЛЛЕЛЬНО (в одном сообщении несколько вызовов Agent), каждый по своей зоне
  (напр. «карта visibility всех таргетов», «кто включает res_tag.h / boost.json», «scene→render usage
  rnd::get_system»). Тип: `Explore` или `general-purpose`. Возвращай выжимку, не дамп.
- **Архитектурные US** (US-49-5 scene↔render, US-49-3 engine_api, US-49-1 границы): привлекай
  `cpp-engine-engineer` / `cpp-editor-architect` для проектирования швов перед реализацией.
- **Независимые срезы:** если US не конфликтуют по файлам — гоняй их анализ параллельно. Если несколько
  агентов МУТИРУЮТ файлы одновременно — используй `isolation: "worktree"`.
- **Механические правки** (массовые однотипные правки CMake/инклюдов): можно делегировать более дешёвой
  модели (sonnet/haiku) с точным списком правок, НО верификацию сборкой держи под своим контролем.
- **Адверсариальная проверка:** для рискованных швов (US-49-5/6/9) проси отдельного агента проверить, что
  поведение/lifetime сохранены.
Правило: после делегирования сам не повторяй ту же работу — дождись результата и синтезируй.

## КОМАНДЫ СБОРКИ И ВЕРИФИКАЦИИ (Windows, Ninja + MSVC)
Сборка идёт через окружение Visual Studio. Существует сконфигуренная папка `build/`.
vcvars: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`.

Реконфигурация + сборка (после правок CMake/исходников):
```
cmd /c "\"<vcvars>\" >nul 2>&1 && cmake \"<repo>\\build\" && cmake --build \"<repo>\\build\" --target unit_tests editor"
```
Прогон тестов (ОРАКУЛ — должно быть `*** No errors detected`, exit 0, ~307 кейсов):
```
<repo>\build\unit_tests.exe
```
Также есть пресеты (US-41-9): `cmake --preset dev-debug` / `--list-presets`. CMake авто-глобит исходники
(CONFIGURE_DEPENDS) — новые .cpp/тесты подхватываются без ручных правок CMake-списков.
В PowerShell: native-stderr НЕ редиректь в `$null` без нужды; here-string для git-сообщений ломается —
коммить НЕСКОЛЬКИМИ `-m`.

## ЖЁСТКИЕ ПРАВИЛА ПРОЕКТА (легко забыть)
- Логирование: только `egLOG`/`egLOG_WARN`/`egLOG_ERROR` (первый арг — категория-строка), НЕ iostream/cout.
- Ассерты: `ASSERT_MSG`/`ASSERT_FAIL` из `engine_assert.h` (но помни: в release они пустые — см. backlog BL-002).
- Ресурсы: только `res::get_system().require<T>(res::tag{...})`, не в обход кеша. `res::tag{"..."}` ДОЛЖЕН
  содержать протокол (`memory://`/`res://`).
- Frame allocator: не хранить указатели между кадрами; `reserve()` перед push.
- ImGui: всегда проверять `Begin()`; панели наследуют `panel_base`.
- ECS: системы — `ecs::system_interface` (auto-register); компоненты POD. Лимит world id 0–4.
- Стиль: C++20, snake_case, tabs(4), `#pragma once`, порядок инклюдов module→stdlib→3rdparty→project.
- Мультимир: три массива `shared_state` строить из одного desc-порядочного источника (`edt::derive_world_arrays`);
  `scn::level::for_each_world` обходит unordered_map в hash-порядке — не полагаться на его порядок.

## РЕКОМЕНДОВАННЫЙ ПОРЯДОК (из ресёрча; каждый шаг — отдельный коммит, behavior-preserving)
1. **Фаза 0 — фундамент:** EPIC-41 US-41-1 (cmake/ThirdParty.cmake + slim Boost через BOOST_INCLUDE_LIBRARIES),
   US-41-4 (visibility specifiers + snk:: алиасы), US-41-6 (imgui отдельный таргет) + EPIC-49 US-49-2
   (`common`→STATIC с mem_*.cpp; убрать `add_subdirectory(${ENGINE_CORE_PATH})` и `target_link_directories`
   из `Editor/code/editor_system/CMakeLists.txt`; editor_system `PUBLIC engine`→`PRIVATE`).
   ⚠️ Visibility-правки ВСКРОЮТ скрытые транзитивные зависимости (ошибки линковки/компиляции) — это ценная
   диагностика; чини по одному таргету, верифицируй сборкой после каждого.
2. **Фаза 1 — дешёвые победы:** US-49-4 (`entt/entt.hpp`→`entt/fwd.hpp` в заголовках; forward-decl `ImTextureID`
   вместо `imgui.h` в публичных хедерах; вынести `<boost/json.hpp>`+`tag_invoke` из `res_tag.h` в `res_tag_json.h`,
   POD вместо glm::ivec2). Низкий риск, режет время компиляции.
3. **Фаза 2 — контракты слоёв:** US-49-1 (public/private include-границы) + US-49-3 (`engine_api`/`core_api`
   INTERFACE-таргеты; агрегаторы `engine`/`core` PRIVATE).
4. **Фаза 3 — wiring:** US-49-7 (`depends_on()` + топосорт в `app_module_loader`) + US-49-8 (реестр стадий
   кадра вместо хардкод-`on_step`; дедуп edt/gs loop).
5. **Фаза 4 — главный шов:** US-49-9 (нарушения слоёв window→gui_gl_backend, scene→gui) → US-49-6
   (инъекция render-менеджеров через frame_context, убрать `rnd::get_system()` из scene/пассов) → US-49-5
   (POD-материалы + render-provider интерфейс) — ПОСЛЕДНИМ, отдельным коммитом, при участии архитектурного агента.

## ДИСЦИПЛИНА И АНТИПАТТЕРНЫ
- Один логический шаг = один коммит; не смешивай visibility-правки и переименования (EPIC-29 US-29-2) — дифф
  станет нечитаемым.
- Не делай агрегаторы `engine`/`core` PUBLIC. Потребители линкуются к `*_api`-контрактам.
- Не разноси физически public/private директории, пока visibility не исправлен (Фаза 0/2).
- US-49-5 (POD-материалы) — самый глубокий шов; не раньше Фазы 4, не в одном коммите с другими US.
- Каждый шаг: `cmake --preset dev-debug && ctest` (307 тестов зелёные) ИЛИ эквивалент через build/.
- Опирайся на существующие эталонные швы: `driver_interface`, `extractor_interface`, `gui_backend_interface`,
  `app_loop_service_interface`.

## КОНТЕКСТ СОСТОЯНИЯ РЕПОЗИТОРИЯ (на момент передачи)
- Ветка `aibranch` (не дефолтная). Коммить только когда пользователь попросит; в конце git-сообщения добавляй
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Возможно НЕ закоммичены (созданы при планировании): `docs/epics/infrastructure/EPIC-49.md`,
  `docs/epics/INDEX.md` (строка EPIC-49), backlog `docs/agent/backlog/` (BL-001..006). Проверь `git status`;
  при необходимости предложи закоммитить план отдельным docs-коммитом.
- Незакоммиченное «не наше» в дереве: сдвинутые сабмодули (assimp/glfw/imgui/yaml), `*.desc`, `claude-framework/` —
  НЕ трогать, стейджить правки точечно (по путям), не `git add -A`.
- Связанный backlog: BL-001 (RT-теги), BL-002 ENGINE_VERIFY (release-asserts), BL-003 CI, BL-004 exception
  periphery, BL-005 frame_allocator_reset leak, BL-006 control_block races — это смежные production-задачи,
  НЕ часть EPIC-49, но знай о них.

## ПЕРВЫЙ ХОД
Начни с DISCOVER по **Фазе 0** (EPIC-41 US-41-1 + US-49-2): запусти параллельных агентов на карту
third-party cache-переменных и visibility всех таргетов, прочитай EPIC-49.md/EPIC-41.md, затем составь
план первого среза и вынеси на CONFIRM. Не пиши код до явного одобрения.
