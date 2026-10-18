# Handoff: Фаза 0 (подготовка движка к ТЗ «Химера») — статус

**Обновлено:** 2026-10-18 (поздний вечер) · **Ветка:** `aibranch` (дефолт — `main`) · **Трекер:** [GitHub Issues](https://github.com/Prorok1015/OpenGL_Engine/issues), [milestones](https://github.com/Prorok1015/OpenGL_Engine/milestones)

Этот файл — точка входа для новой сессии: прочитав его, можно продолжить работу.

## 1. Контекст и ключевые документы

| Документ | Что в нём |
|---|---|
| [docs/roadmap/CHIMERA_ROADMAP.md](../../roadmap/CHIMERA_ROADMAP.md) | Разрывы между ТЗ «Химера» и движком, ADR-01…13, вехи M1–M6 и этап Б, эпики EPIC-50…76 |
| [docs/roadmap/ARCHITECTURE_REVIEW.md](../../roadmap/ARCHITECTURE_REVIEW.md) | Оценка архитектурных решений, баги, легаси, план фазы 0 (§7), **принятые решения владельца (§9)** |
| [docs/roadmap/LEGACY_TEMPLATES.md](../../roadmap/LEGACY_TEMPLATES.md) | Инвентаризация шаблонных шейдеров/уровней/ассетов — ждёт решений владельца |
| [docs/agent/tracker/manifest.json](../tracker/manifest.json) | Исходный набор issues (ключ → номер) |
| [docs/epics/INDEX.md](../../epics/INDEX.md) | Дизайн-документы эпиков (статус — в Issues) |

## 2. Принятые решения (кратко, подробно — ARCHITECTURE_REVIEW §9)

- Платформа сейчас Windows + OpenGL; RHI проектируется так, что новый графический API = новый бэкенд. Проверка — null/validation-бэкенд.
- GLFW остаётся, но за интерфейсом `platform::window`; GL-контекст переезжает в `drv_opengl`.
- Только собственные графические бэкенды (без SDL_GPU и подобных).
- Шейдеры — GLSL с правилами переносимости (`#version 450`, только блоки, `SNK_BINDING(set,slot)`, явные ключи вариантов). Slang — пересмотреть при не-GL бэкенде.
- Явные фазы ECS по схеме §9.5 (фаза — свойство системы в коде, набор систем — в `world_desc`, tie-break по порядку desc, `run_in_edit`).
- Приватные члены — префикс `m_`.
- GitHub Issues — основной трекер; `refs #N` / `closes #N` в коммитах.
- Тестовые модели — в сабмодуле `Editor/res/samples` ([OpenGL_Engine_samples](https://github.com/Prorok1015/OpenGL_Engine_samples), `update = none`); `robot.glb` остаётся в движке. История git переписывается один раз при мерже `aibranch` → `main`.

## 3. Что сделано

**Этап A (расчистка):** −1100 строк легаси, исправлена документация агентов (`CLAUDE.md`, `docs/agent/*.toon`), `guidelines.md` удалён, CI (GitHub Actions), инвентаризация шаблонов.

**Этап B (корректность):** планировщик ECS (топологический порядок, системы всех миров при reload), потокобезопасность ресурсов (BL-006), ошибки загрузки desc вместо падений, ассерты на `NDEBUG` + `ENGINE_VERIFY`, периметр исключений (BL-002/004), аллокатор и сброс кадра (BL-005), баги редактора (commit по отпусканию, Save & Continue).

**Инфраструктура качества:**
- CI: Windows MSVC Debug/Release, Linux GCC/Clang (все блокирующие), ASan+UBSan, TSan, coverage (неблокирующий, HTML-артефакт). Релиз редактора по тегу `v*` (`release.yml`).
- Тесты: `tests/` (было `unittests/`), ~596 кейсов, покрытие строк 16% → 35%.
- Headless-запуск редактора без GPU: `tools/run_headless.sh` (Xvfb + Mesa llvmpipe, xdotool, скриншоты), сценарии `tools/headless/*.actions`, уровень `Editor/res/levels/smoke_level.desc`, опция `--set editor/startup_level levels/smoke_level.desc`.
- Ручной чек-лист рендера после EPIC-49 пройден headless (9/9).

**Баги, найденные тестами/QA:** исправлены №3–15, №17 (и race логгера). Открыты: №1, №2 (sentinel-трансформы, поверхностный merge — решаются эпиком «Форматы данных»), №16, A (правка света в инспекторе гасит свет), B (сохранение теряет transform glb-узла — потеря данных), C (редкий краш при старте), D (мультимир показывает чужой мир).

## 4. Этап C — прогресс (2026-10-18)

**Сделано (в `aibranch`):**
- Баги #81–#84, #169, #171, #173, #175, #177, #178, #179, #181, #182; краш Clang 18; закрытие файла в резолвере до сигнала (Windows).
- EPIC-29 (#6) целиком: #58, #60 (+#170), #61, #62, #59 (PIC/visibility/shared `snk_engine`). Дизайн: `docs/roadmap/design/EPIC-29_SERVICES_DLL.md`.
- EPIC-50 (#8) явные фазы ECS: #89–#95, #79; камера в `pre_update` (#179).
- Форматы данных (#7): #86, #87, #63, #88, #85 (записи overrides + глубокое слияние, prefab/world v3, флаг `has_transform`) → #64, #65. Дизайн: `docs/roadmap/design/DATA_FORMATS_V2.md`.
- Ресурсы слоями #176 (+#53): `core/res` → `Editor/res`.
- CMake/CI (#11): #107, #106, #108 SDK `snake_sdk` (find_package, сторонние внутри, static+shared, CI-потребитель), #180 ускорение (ccache/sccache, PCH, `SNK_ASSIMP_FORMATS`, пресеты `ci-*`: CI ~21 мин → единицы минут), #54, #56 визуальный smoke в CI.
- Хвосты A/B (#4): `LEGACY_TEMPLATES` решены, `CMakeUserPresets.json` удалён, чек-лист Windows в #55.

- Форматы данных (#7) закрыт целиком: + #192 редактор по `node_id`, инспектор пишет override-записи («Reset to base»), #172; #195 база инспектора при вложенных инстансах (кэш `override_edit::view_cache`: `shared_ptr<const T>`, ключ мир/owner node_id/цель + `shared_state::desc_generation`).
- #9 цикл кадра (#96–#100): именованные стадии, один `app::loop_service`, цель рендера камеры. #190 Game view в редакторе.
- #186 hot reload сохраняет миры, созданные в редакторе (редактор ресинхронизируется после любой перезагрузки); #196 уровень без миров (`active_world_idx = -1` допустим).
- #198 Windows MSVC shared в CI (`snk_engine.dll`; imgui экспортируется из DLL через `IMGUI_API`; Boost.JSON/assimp статически в editor — безопасно при `/MD`).
- #10 платформенный шов, дизайн утверждён: `docs/roadmap/design/PLATFORM_SEAM.md` (свой ImGui-бэкенд на событиях `wnd`, swap/vsync/glad в драйвере через `wnd::gl_surface`, курсор в координатах framebuffer). Шаги #101 (интерфейсы `wnd` без GLFW, `null_window_system`), #102 (swap/vsync/glad в драйвере через `wnd::gl_surface`, `gl::swapchain`), #103 (события char/framebuffer/scale, курсор в координатах framebuffer, `edt::make_input_area`, пересборка шрифта по смене scale, cursor shape/clipboard у окна) и #104 (своя ImGui-платформа `gui::platform_backend` на событиях `wnd`, `imgui_impl_glfw` удалён, геймпад-навигация снята) — сделаны; **эпик #10 закрыт**.
- #201 SDK shared на Windows (CI: `SDK package (Windows shared)`, `Windows MSVC Shared Release`; `IMGUI_API=dllimport` для потребителей пакета).
- #202 браузер ассетов по слоям ресурсов (`resource_system::get_resource_layers`, `edt::list_merged_directory`; нижние слои приглушены) + отказ смены мира оставляет прежний активный мир.

- #211 планировщик уровня (фазы по id, сброс fixed-аккумулятора, контракт reload: per-world экземпляры пересоздаются); #207 viewport в пикселях framebuffer.
- #109 подготовка: PR-0 (`snk_component_headers`, install двух раскладок, `tools/ci/check_sdk_headers.sh`) и инструмент `tools/header_split/` (дизайн `docs/roadmap/design/HEADER_SPLIT.md`, утверждён).

**В работе (2026-10-18 вечер):** #109 PR-1 (application/core_input/window), далее PR-2…PR-5 строго по очереди; #216 баг std140 в UBO; дизайны волны 0 этапа D (EDITOR_FRAMEWORK, DESC_SCHEMA, TASK_SYSTEM).

**Осталось по этапу C:** #109 PR-1…PR-5; #174 предупреждения (сразу после PR-5); #209 клавиши; тест ключей кэша инспектора; #105 (ждёт EPIC-34).

**Решение владельца — ресурсы слоями:** в ядре (`core/res`) базовые ресурсы, чтобы модели рисовались «из коробки» в любом новом проекте; редактор и игра добавляют шаблоны и переопределения; `res://` ищется проект → редактор → ядро.

**Этап D подготовлен:** план `docs/roadmap/STAGE_D_PLAN.md` (утверждён, §5); дизайны `RHI.md` и `WORLD_CONTEXT.md` утверждены (#114, #126 закрыты как выполненные; #119 разделён → #229). Код D — после PR-5 #109 и #174.

**Дистрибуция:** эпик #217 (дизайн `docs/roadmap/design/DISTRIBUTION.md`, утверждён): игра как библиотека (`find_package(snake_sdk)`) и как `game.dll`-модуль редактора; DebugGame; только Windows; MSVC ≥ 14.44; долгосрочно — wasm-слой игрового кода (§7, spike #230). Первая story — spike EnTT между DLL #218.

Затем этап D (#12–#18) и вехи M1–M6 по ТЗ.

## 5. Ключевые issues

Всего заведено #4–#170 (полный список «ключ → номер» — `docs/agent/tracker/manifest.json`): 49 эпиков, 95+ stories, баги. Ссылки `**Issue:** #N` проставлены в `docs/epics/**/EPIC-*.md`, таблицы — в ARCHITECTURE_REVIEW §7 и CHIMERA_ROADMAP §5.

| Что | Issues |
|---|---|
| Хвосты этапов A/B | #4 |
| Стабилизация: баги фазы 0 (группирующий эпик) | #5 — исправленные закрыты со ссылкой на коммит; открытые: #79 (бывш. №16, ребро графа систем), #81 (A, свет в инспекторе), #82 (B, save теряет transform — потеря данных), #83 (C, редкий краш старта), #84 (D, мультимир), #169 (RT-теги без протокола) |
| Баги №1, №2 (merge overrides) | #64, #65 → эпик «Форматы данных» #7 |
| Этап C | #6 EPIC-29 сервисы/DLL (+#170 skinning_manager), #7 форматы данных, #8 EPIC-50 явные фазы ECS, #9 цикл кадра и player, #10 платформенный шов, #11 остаток EPIC-41 (#105–107) |
| Этап D | #12 world_context, #13 RHI шаги 1–4, #14 правила GLSL, #15 POD-пакеты рендера, #16 EPIC-55 схема desc, #17 EPIC-64 task_system, #18 каркас редактора |
| M1 Проект и игровой модуль | #19 EPIC-42, #20 EPIC-36, #21–#23 (EPIC-52/53/54) |
| M2 Данные | #24–#28 |
| M3 Документный редактор | #29–#38 |
| M4 / M5 / M6 / Этап Б | #39–#41 / #42–#45 / #46–#49 / #50–#52 |

Замороженные эпики (EPIC-31, 32, 33, 35, 40, 46, 47, 48) в Issues не заведены — помечены `frozen` в `docs/epics/INDEX.md`.

## 6. Как работать (договорённости)

- Процесс: DISCOVER → PLAN → CONFIRM (явное «да» владельца) → IMPLEMENT → VERIFY → REPORT (`docs/agent/workflow/`).
- Оркестрация: реализацию отдавать Sonnet-агентам (эксперимент с Haiku завершён — владелец выбрал Sonnet); крупную работу делегировать в отдельных git worktree; **агент первым шагом делает `git switch -c <branch> aibranch`** (worktree по умолчанию стартует от `origin/main`). Изменения в основное дерево — cherry-pick/fast-forward после сборки и тестов.
- Корректность/конкурентность — отдельное ревью вторым агентом.
- Файлы с CRLF правятся только инструментом Edit (не python/sed с перезаписью).
- `closes #N` закрывает issue только при попадании в `main` — в `aibranch` закрывать вручную.
- Проверка через CI (решение владельца 2026-10-18): агент пишет код в ветке `wip/<issue>-<slug>` от `aibranch`, пушит её и открывает **draft PR в `aibranch`** (PR-прогоны берут кэш компиляции из `aibranch`). Локально не собирает. Оркестратор следит за CI PR, ревьюит, затем переводит PR из draft в ready и делает **squash-merge в `aibranch` через GitHub** (решение владельца 2026-10-18; ветка удаляется автоматически настройкой репозитория). Удалять ветки из сессии нельзя — git-прокси обрывает push с удалением. CI покрывает Linux GCC/Clang/ASan/TSan/shared, Windows MSVC Debug/Release, headless smoke с проверкой кадра (`check_smoke.sh`).
- Ревью и мерж (владелец 2026-10-18): заметные PR — независимое ревью вторым агентом (только `git show/diff`, без checkout в общем дереве), находки — обратно автору-агенту; PR с чистым ревью и зелёным CI мержится. CodeRabbit не используется (решение владельца: ревью делают сабагенты оркестратора; бесплатный CodeRabbit — ≤1 ревью/час, вручную). Если PR тестировался на старой базе и пересекается с новыми мержами — вмержить `aibranch` в ветку и дождаться CI. CI теперь покрывает и Windows MSVC Shared.
- Проверка перед пушем в `aibranch` (если что-то правится прямо там): сборка + `unit_tests` + headless-запуск `smoke_level`.
- Коммиты оканчиваются строками `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` и `Claude-Session: <url сессии>`.

## 7. Окружение облачной сессии (контейнер эфемерный — восстанавливать)

```bash
git submodule update --init --depth 1            # кроме Editor/res/samples (update = none)
sudo apt-get install -y libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
     ccache xvfb libgl1-mesa-dri xdotool imagemagick
cmake --preset ci-linux-gcc-debug                 # build/ci-linux-gcc-debug; ccache и PCH включены самим CMake (#180)
cmake --build build/ci-linux-gcc-debug --target editor unit_tests && build/ci-linux-gcc-debug/unit_tests
tools/run_headless.sh --editor build/ci-linux-gcc-debug/Editor/editor --seconds 25 --shot headless_out/smoke.png -- --set editor/startup_level levels/smoke_level.desc
```
Скорость сборки (#180) — опции CMake, поэтому одинаково для локальной сборки, IDE и CI: `SNK_COMPILER_CACHE=AUTO|ON|OFF`
(ccache, на Windows sccache; свой `CMAKE_<LANG>_COMPILER_LAUNCHER` имеет приоритет) и `SNK_PCH=AUTO|ON|OFF` (общий PCH
`cmake/pch/snk_pch.h`; AUTO = ON, кроме MSVC с кэшем — sccache не кэширует `/Yu`). Пресеты `ci-*` в `CMakePresets.json` — ровно
конфигурации из `.github/workflows/build.yml` (`ci-linux-{gcc-debug,clang-debug,asan,tsan,coverage,shared}`,
`ci-windows-msvc-{debug,release}`); `ci-linux-clang-debug` собирается с `SNK_PCH=OFF` (ловит include, спрятанные PCH).
Лёгкая проверка без PCH: `cmake --preset dev-debug -DSNK_PCH=OFF`. Каталог кэша контейнера: `CCACHE_DIR=/home/user/.ccache`.
Старые сборки без пресетов (`cmake -S . -B build/linux ...`) работают как раньше.
Санитайзеры: пресеты `ci-linux-asan` / `ci-linux-tsan` (или `-DSNK_SANITIZE=address|thread`). Покрытие — job `coverage` в `.github/workflows/build.yml` (пресет `ci-linux-coverage`).
