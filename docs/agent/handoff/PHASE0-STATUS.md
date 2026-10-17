# Handoff: Фаза 0 (подготовка движка к ТЗ «Химера») — статус

**Обновлено:** 2026-10-17 · **Ветка:** `aibranch` (дефолт — `main`) · **Трекер:** [GitHub Issues](https://github.com/Prorok1015/OpenGL_Engine/issues), [milestones](https://github.com/Prorok1015/OpenGL_Engine/milestones)

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

## 4. Что дальше

1. **Этап C** (milestone «Фаза 0 — этап C»): EPIC-29 сервисы и готовность к DLL; «Форматы данных» (`node_id`, `__version`, `__guid`, presence-трансформы); EPIC-50 явные фазы ECS; цикл кадра и player; платформенный шов; остаток EPIC-41; баги A–D. Начинать после явного «да» владельца.
2. **Этап D** (milestone «Фаза 0 — этап D»): `world_context`, RHI шаги 1–4 + null-бэкенд, правила шейдеров, POD-пакеты рендера, схема desc (EPIC-55), `task_system` (EPIC-64), каркас редактора.
3. Затем вехи M1–M6 по ТЗ.

Хвосты этапов A/B — эпик [#4](https://github.com/Prorok1015/OpenGL_Engine/issues/4): решения по LEGACY_TEMPLATES, `<iostream>` в `common.h`, ручная проверка на Windows, визуальный smoke-тест в CI, переписывание истории при мерже в main.

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
- Оркестрация: крупную работу делегировать Sonnet-агентам в отдельных git worktree; **агент первым шагом делает `git switch -c <branch> aibranch`** (worktree по умолчанию стартует от `origin/main`). Изменения в основное дерево — cherry-pick/fast-forward после сборки и тестов.
- Корректность/конкурентность — отдельное ревью вторым агентом.
- Файлы с CRLF правятся только инструментом Edit (не python/sed с перезаписью).
- Коммиты оканчиваются строками `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` и `Claude-Session: <url сессии>`.

## 7. Окружение облачной сессии (контейнер эфемерный — восстанавливать)

```bash
git submodule update --init --depth 1            # кроме Editor/res/samples (update = none)
sudo apt-get install -y libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
     ccache xvfb libgl1-mesa-dri xdotool imagemagick
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
ninja -C build/linux editor unit_tests && (cd build/linux && ./unit_tests)
tools/run_headless.sh --seconds 25 --shot headless_out/smoke.png -- --set editor/startup_level levels/smoke_level.desc
```
Санитайзеры: `-DSNK_SANITIZE=address` (ASan+UBSan) или `thread`. Покрытие — job `coverage` в `.github/workflows/build.yml`.
