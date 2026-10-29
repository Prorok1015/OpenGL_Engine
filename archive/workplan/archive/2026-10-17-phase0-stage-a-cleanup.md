# Workplan: Фаза 0, этап A — расчистка легаси и документации + CI

**Slug:** phase0-stage-a-cleanup
**Task type:** refactor (behavior-preserving deletions) + chore (CI)
**Started:** 2026-10-17
**Current phase:** IMPLEMENT
**Status:** in-progress

## Task
По docs/roadmap/ARCHITECTURE_REVIEW.md §6, §7 (этап A), §9.6–9.7: удалить безрисковое легаси, поправить устаревшую документацию,
подготовить таблицу по шаблонным шейдерам/уровням (без удаления), добавить GitHub Actions (сборка в конфигурациях + релиз редактора).
Одобрено владельцем: «Принимаю… принимайся за расчистку. Делай через соннет агентов» + запрос на CI.

## Plan
Батчи (Sonnet-агенты, непересекающиеся файлы, без коммитов — коммитит оркестратор после проверки):
- A1 core/app/ecs/image легаси
- A2 render/gui/input легаси
- A3 resource/desc легаси
- A4 editor легаси
- A5 ассеты и репо (.idx, core/res, test assets) + инвентаризация шаблонов (read-only отчёт)
- A6 документация (ImGui-правило, get_system, m_, guidelines.md, примеры, счётчики)
- CI: .github/workflows

Verification: Linux Debug сборка (Ninja, GLFW_BUILD_WAYLAND=OFF) — не должно появиться новых ошибок относительно базовой;
unit_tests где собираются. Windows/MSVC — через CI.

## Out of scope (этап B/C)
Баги корректности, перенос регистраций из gs_game_init, ассерты, CMakeUserPresets.json (личный файл владельца — не трогать).
