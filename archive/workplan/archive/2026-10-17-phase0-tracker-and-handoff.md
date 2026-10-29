# Workplan: Фаза 0 — перенос задач в GitHub Issues + handoff

**Slug:** phase0-tracker-and-handoff
**Task type:** other (tracker/docs)
**Started:** 2026-10-17
**Current phase:** IMPLEMENT
**Status:** done (2026-10-17)
**Durable copy:** docs/agent/handoff/PHASE0-STATUS.md (committed) — read it first after a restart/compaction.

## Где мы (2026-10-17)
- Фаза 0, этапы A и B — ВЫПОЛНЕНЫ, запушены в aibranch (CI зелёный на Win MSVC Debug/Release, Linux GCC/Clang, ASan+UBSan, TSan; coverage job non-blocking).
- Найденные баги №3–15, №17 исправлены; открыты №1, №2 (этап C, модель префаба), №16, A (свет в инспекторе), B (save теряет transform glb-узла), C (редкий краш старта), D (мультимир).
- Тесты: tests/ (переименовано из unittests/), 596 кейсов. Headless-редактор: tools/run_headless.sh (+ tools/headless/*.actions, levels/smoke_level.desc, --set editor/startup_level <path>).
- Модели вынесены в сабмодуль Editor/res/samples (repo Prorok1015/OpenGL_Engine_samples, update=none); robot остался.

## Текущая задача (утверждена владельцем)
GitHub Issues — основной трекер (эпик → нативные sub-issues, баги с меткой bug, метки/milestones 1–9).
- [x] Плейн утверждён («создавай»); milestones 1–9 созданы владельцем (tools/github/create_milestones.*), проверены (1=«Фаза 0 — этап C», 9=«Этап Б (ТЗ)»).
- [x] Манифест docs/agent/tracker/manifest.json (165 записей: 49 эпиков, 95 stories, 21 баг) — ветка docs/github-tracker (worktree agent-a5f749fe68dcf0661).
- [x] Эпик #4 «Фаза 0: хвосты этапов A/B» создан, milestone 1.
- [x] Sonnet-агент завёл issues #4–#168 (выборочно проверено: 165 шт., дублей нет; #5 — 19 детей) — cherry-pick 551f4ff. Заводит остальные issues + ссылки в docs (EPIC-*.md **Issue:**, INDEX.md + frozen, ARCHITECTURE_REVIEW §7, CHIMERA_ROADMAP) → коммит в docs/github-tracker → я cherry-pick в aibranch и пушу.
- [x] CLAUDE.md раздел «Task Tracking — GitHub Issues», SYNC.toon [PROJECT_POLICY], INDEX.toon sync_target: github (коммит 6bedbe2).
- [x] (db7ae9c) docs/agent/handoff/PHASE0-STATUS.md с номерами ключевых issues/milestones.
- [x] Worktree agent-a8b1a5596c8e7ae9d удалён (QA-скрипты уже в aibranch; repro-уровни бага A сохранены в scratchpad/qa_repro/).
- [x] (db7ae9c) issues #169 (BL-001), #170 (BL-007); worktree удалён. После агента: удалить worktree agent-a5f749fe68dcf0661; завести issues BL-001 (bug, RT-теги без протокола) и BL-007 (tech-debt, sub-issue EPIC-29) — в манифесте их нет; BL-008 покрыт US-41-7/8/10; пометить BL-001/007/008 в INDEX.toon external_id.
- [x] CI зелёный целиком на 539b3c3 (run 25: Win MSVC Debug/Release, Linux GCC/Clang, ASan+UBSan, TSan, coverage); #171 закрыт вручную (closes #N не срабатывает вне main). Было: CI 4c082c4: TSan красный — реальный race `m_on_resource_changed` (res_resolver_interface.hpp:34 vs поток vfs resolver; прод-путь res_system.cpp:24). Sonnet-агент чинит в worktree, ветка fix/tsan-resolver-callback → cherry-pick, завести bug-issue (closes #N), push. ASan ✅, coverage ✅ (отчёт сформирован). MSVC Debug/Clang падали на неоднозначном get_world(0u) в tests/scn_assembler_level_tests.cpp:417 — исправлено 5ac776b, запушено; ждать CI. В build.yml добавлен paths-ignore docs/**, **/*.md.
- [ ] Спросить владельца: удалить устаревший docs/agent/handoff/EPIC-49-agent-brief.md?

## Дальше (после трекера) — ждёт «да» владельца
Этап C (milestone 1): сервисы/DLL (EPIC-29), форматы данных (node_id/__version/__guid; закрывает баги №1–2), явные фазы ECS (EPIC-50, §9.5), цикл кадра + player, платформенный шов, остаток EPIC-41; плюс баги A–D.

## Инфраструктура сессии (контейнер эфемерный!)
- Сборка: /home/user/.deps/snk_build.sh <src> [targets]; тесты: /home/user/.deps/snk_test.sh <src>; worktree setup: /home/user/.deps/snk_wt_setup.sh <wt> (симлинки сабмодулей). Boost src /home/user/.deps/boost-src, ccache /home/user/.ccache. Пакеты apt: xvfb mesa xdotool imagemagick ccache, X11 dev headers.
- Агенты в worktree стартуют от origin/main — первым шагом `git switch -c <branch> aibranch`.
- Коммиты: футер Co-Authored-By: Claude Opus 5.5 + Claude-Session; CRLF-файлы править только Edit.
