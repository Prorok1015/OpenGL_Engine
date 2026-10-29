# СНИМОК ДЛЯ КОМПАКТА (2026-10-27 ~13:00Z)

## Процесс (без изменений)
- Агенты пишут на wip/*, черновые PR в aibranch; независимое ревью (security-ревью для путей/MCP/конкурентности); снять draft → полный CI 12 job; проверить smoke "reference diff" ×5; merge-tree/overlap → при пересечении влить aibranch в ветку PR; squash с моим commit_message (trailers Opus 5.5 + Claude-Session); закрыть issue вручную; unsubscribe; git pull --ff-only.
- Логи CI: curl api.github.com/.../commits/<sha>/check-runs, /actions/jobs/<id>/logs. Артефакты smoke: `gh run download <run> --name editor-smoke` (gh работает) → смотреть PNG через Read.
- Push+undraft одновременно → full-tier прогон может отмениться (concurrency): проверять и перезапускать отменённый run (rerun_workflow_run).
- Эталонные PNG — только решением владельца. Владельцу отвечаю по-русски.
- Владелец теперь может работать и через свои локальные агенты (я пишу им сообщения), но последнее решение: M1 делают МОИ облачные агенты в отдельных сессиях.

## Состояние
- Фаза 0: код завершён, эпик #11 закрыт (split заголовков #364/#365), #55 демо закрыт (итог-комментарий), #292 закрыт. Остался #57 (переписать историю git перед мержем в main — владелец; до этого замер размера и список путей). #123 отложен.
- Демо-баги: #369 (#375), #370 (#376), #371+#373 (#374) — влиты локальными агентами владельца (merge-коммиты), issues закрыты мной.
- Release для демо: ветка wip/demo-release (release.yml не в main → workflow_dispatch недоступен; триггер на push в эту ветку). DEMO.md самодостаточен.

## В работе (здесь)
- PR #377 wip/vfs-safe-path-race — aibranch КРАСНЫЙ (Linux GCC no Assimp, StoreEraseStoreErase): safe_path делал weakly_canonical на сам файл → гонка с erase на io-потоке. Фикс: канонизировать только каталог + отказ от symlink в последнем компоненте + тест 300 циклов; 2-й коммит 92d33ec: retry rename (Windows delete-pending, "Access is denied"). Ждёт CI на 92d33ec → смёрджить (refs #361).
- PR #378 wip/368-camera-gizmo (#368): причина — inject_camera ставил world_transform=identity, никто не пересчитывал до движения камеры. Фикс: seed world_transform из позы. smoke.png 0.224% ок; checklist.png 23.7% — эталон снят С БАГОМ (первое движение прыгало в дефолтную позу 0,2,5 yaw45). Агент (subagent aa738e3230ec185b5) возобновлён: дефолт = поза в 0 с identity, НО ненулевая дистанция орбиты; тесты на зум/орбиту без прыжка; прислать все 5 процентов. Потом показать владельцу новый checklist vs эталон, рекомендовать обновить эталон (его решение).
- PR #379 wip/372-ui-font (#372 код шрифта): ревью APPROVE, undraft, full CI идёт → мерж (refs #372). Затем владелец через ЛОКАЛЬНОГО агента добавляет core/res/fonts/NotoSans-Regular.ttf + OFL.txt + тест глифа U+0416 (сообщение уже дано, с шагом 5), closes #372; тогда смотреть smoke % и спросить владельца про эталоны.

## M1 (запущено 2026-10-27, отдельные облачные сессии, отчёт через комментарии в issues)
- session_01KQMEsfsk9Nas5u67XCwKPS — EPIC-36 edt:// #161–#165 (sonnet), ветки wip/161..165-*, PR по историям → мне ревью/мерж.
- session_01KQMEsjxYynvremtRtvW3ih — спайк #218 EnTT+DLL, wip/218-dll-spike, PR "[spike, do not merge]", отчёт docs/roadmap/design/SPIKE-218-entt-dll.md → резюме владельцу.
- session_01KQMEsg4YXv7Uwp2sRV2xsx — план EPIC-42, wip/19-project-system-plan, docs/roadmap/design/EPIC-42-PLAN.md → вопросы владельцу на подтверждение, код только после «да».
- Проверка сессий: send_later trig_01BbBTfgB8AjF3AGwuHSTPjL на 13:32Z (get_session, новые PR, комментарии в #20/#161-165, #218, #19).
- После плана EPIC-42 и спайка: EPIC-53, EPIC-54.

## Открытые хвосты
- #363 (пути сохранения редактора мимо safe_path, P2), #283 п.6 (Vulkan), #291 (ждёт undo #29), #57 (владелец).

# >>> СНИМОК ДЛЯ КОМПАКТА (2026-10-20 ~11:25Z) — читать первым <<<
aibranch HEAD: 0f809dc (docs handoff) поверх 1e362ad. Все PR этого дня слиты; открыты только агентские ниже.
Процесс мержа: draft → быстрый (5) + независимое ревью (security-ревью для MCP/путей/конкурентности) → снять draft → полный (12) → smoke «reference diff» 0.000% ×5 + merge-tree clean + overlap-check → squash С МОИМ commit_message (трейлер Opus 5.5 + Claude-Session; агенты иногда ставят чужой трейлер — squash это исправляет) → закрыть issue вручную с комментарием → unsubscribe → git pull --ff-only.
Частые причины красного CI: SNK_API у классов/функций, которые зовут тесты (shared только в полном); -Werror (SNK_WARNINGS_AS_ERRORS=ON с #344); ninja останавливается на первой ошибке — после фикса ищи такие же места grep'ом.
В работе (агенты, CI-only, без локальной сборки):
  - #322 вариант B (решение владельца): editor_services у editor_system — агент a18720bea0f05623f, wip/322-editor-services.
  - #352 wireframe только во вьюпорте редактора (решение владельца/моё): агент ae4141047fbf53305, wip/352-editor-wireframe. Параллельно с #322 — при мерже второго влить aibranch.
  - #105 assimp опциональный (SNK_WITH_ASSIMP, no-assimp CI job): агент a5861a61d83128cde, wip/105-assimp-optional.
АУДИТ ГОТОВ (11:35Z): закрыты #229 #136 #139 #135 #110 #15 #13 #16 #17 #18 #12 #14. Этап D: открыт только #123 (отложен). Этап C: открыты #105 (в работе), #11 (эпик, после #105; критерий «public/-сплит» проверить), #5 (все подзадачи закрыты; владельцу подтвердить «краш C»), #4 (#55 #57 — владелец).
РЕШЕНИЯ ВЛАДЕЛЬЦА (2026-10-20 ~11:50Z): #302 — вариант Б (алиас = ребро графа), агент wip/302-alias-edge запущен; #5 закрыт; #57 — ждёт мержа в main (перед ним замер размера и список путей); демо #55 — только по команде владельца, после всех остатков; Release workflow перед демо.
  - send_later 12:00Z: проверить push-CI aibranch после трёх хвостов (#346/#347/#348 вместе не тестировались).
Слито сегодня (день+вечер): #335 #334 #339(R7c, закрыты #118 #119) #336 #337 #338(#321) #340(#131) #341(A4 #290) #342(#120 #122) #344(-Werror #258) #345(#307; #306 dup #331) #346(#326) #347(#333; #280 частично) #348(#343 #277 #236).
Открытые хвосты: #280 (set_jobs в ctor), #283 п.6 (per-view кольцо, Vulkan), #349 (освобождать заменённые программы), #350 (build_remap зацикливание, bug), #351 (resource_system::erase + утечка отменённого импорта), #302 (семантика register_alias), #123 (отложен).
AI: A4 готов; A5 #291 ждёт undo #29 (M3); A6b #293 / A7 #294 — после A5.
Владелец: #55 демо (DEMO.md, шаг 16 — MCP #292), #57 переписать историю при мерже в main; перед демо запустить Release workflow на aibranch.
# <<< конец снимка >>>

> **ПРАВИЛО ВЛАДЕЛЬЦА (2026-10-18, напомнено 2026-10-19): агенты НЕ собирают и НЕ запускают ничего локально — ни cmake, ни build, ни тесты, ни Xvfb. Только push в wip-ветку → draft PR → CI. Каждый бриф обязан содержать эту строку. CI бесплатный — гонять смело, параллельно и сколько нужно; диск не ограничивает число агентов.**

# Workplan: Фаза 0 — этап C (+ подготовка этапа D)

**Slug:** phase0-stage-c · **Approved:** «да» на этап C (2026-10-17) · **Current phase:** IMPLEMENT
**Durable copy:** docs/agent/handoff/PHASE0-STATUS.md · трекер: milestone «Фаза 0 — этап C»
**HEAD aibranch:** 2a22205 (docs WORLD_MESSAGING утверждён) поверх 1b7c3dc (#266) и 93bd8b5 (#257). Push-CI aibranch не проверялся отдельно после последних мержей — проверить.

## Схема работы (решения владельца 2026-10-18)
- Агенты (Sonnet, worktree) НЕ собирают локально. Ветка `wip/<issue>-<slug>` от aibranch → push → **draft PR в aibranch**; CI через pull_request. Worktree-агентов запускать, когда cwd = /home/user/OpenGL_Engine (иначе «not in a git repository»).
- Подписка на PR (subscribe_pr_activity) → зелёный CI → **независимый ревьюер-сабагент** (read-only: только git show/diff/grep, никаких checkout в основном checkout) на каждый нетривиальный PR; находки — автору (SendMessage тому же agentId); мелкие PR (≤ ~40 строк) могу ревьюить сам. CodeRabbit не используем.
- Чистое ревью + зелёный CI → ready → squash-merge (merge_pull_request, squash, expectedHeadSha) → закрыть issue вручную (база не default) → `git pull --ff-only origin aibranch`.
- PR тестировался на старой базе с пересечением файлов/CMake → вмержить aibranch в wip (merge-коммит) и ждать CI; без пересечения (проверка `comm` по diff --name-only) — можно мержить. pull_request-CI гоняет merge-ref с базой на момент пуша.
- Перед своим коммитом — `git branch --show-current` == aibranch. Прямые правки aibranch — только docs.
- Агенты типа Plan не могут писать файлы — текст приходит в отчёте; извлекаю python-скриптом из output-файла задачи (не читая его целиком) и коммичу.
- Агенты-авторы иногда держат фоновые мониторы и шлют повторные отчёты — говорить «stop polling», при необходимости TaskStop.
- Ветки удаляет владелец (автоудаление после мержа, похоже, включено).
- send_later для проверки push-CI aibranch (его события не приходят).

## В работе (агенты в фоне) — снимок 2026-10-19 ~18:25Z, HEAD aibranch 63939ad (f00ea61 + docs)
Подписка: на КАЖДЫЙ новый PR сразу. Процесс мержа (#310): draft зелёный (5 проверок) + ревью → снять draft → ПОЛНЫЙ прогон (11, бенч skipped) → проверить smoke «reference diff» 0.000% + merge-tree clean → squash (expectedHeadSha) → закрыть issue вручную → unsubscribe → `git pull --ff-only`. Ветки без #310 при снятии draft не запускают полный прогон — влить aibranch (реальный merge, я могу сам в /tmp-scratchpad worktree). Агенты, сдавшие отчёт и ждущие CI, — TaskStop (я подписан). Все агенты CI-only. CodeRabbit-комментарии игнорировать. Safety-net send_later ~50 мин (последний trig_016zxHt1jxPQp6Lo1hhMvRj5 на 18:19).
Влито сегодня после компакта: #278 PR-6 WORLD, #309 W2, #298, #296 R5b, #310 CI-уровни, #299, #304, #308 (+checklist.png по решению владельца), #312 W3, #305 A2. Закрыты: #270 #209 #117 #121 #212 #276 #286 #263 #232 #130 #271 #288. Заведены: #311 (рёбра графа не чистятся), #314 (desc::diff абс. допуск float).
| PR / ветка | агент | состояние |
|---|---|---|
| ✓ #313 R6a СЛИТ c2c86c7 (18:22Z), #124 закрыт; лимит шага санитайзеров теперь 25 мин. R6b #125 и W4 #272 ЗАПУЩЕНЫ (после компакта) |
| ✓ #318 R6b СЛИТ 8a4595e (20:05Z), #125 закрыт, отписан. TODO: таблица «После R6» в RHI.md с push-прогона бенчмарка на aibranch |
| ✓ #319 W4 СЛИТ d3ebbe8 (22:21Z), #272 закрыт, отписан. checklist.png 1.287% — ждёт решения владельца; хвост: мёртвый apply_viewports |
| ✓ #316 СЛИТ d59978f (18:43Z), #137 закрыт, отписан |
| ✓ #320 PR-9 СЛИТ cf8f4c6 (19:56Z), #140 закрыт, отписан; хвосты → #322 |
| ✓ #317 A3 СЛИТ 18e53c9 (20:28Z), #289 закрыт, отписан. Бенчмарк «После R6» в RHI.md — docs ec53021 |
| ✓ #325 A6a СЛИТ a2501de (00:25Z); #292 открыт до ручной проверки Claude Code/MCP Inspector (владелец/демо); остаточные риски → issue (tech-debt) |
| ✓ #332 СЛИТ 29d1b3b (00:55Z), #331 закрыт |
| ✓ #323 СЛИТ f0825fd (21:20Z), #314 закрыт, отписан |
| ✓ #324 СЛИТ 90aae90 (22:20Z), #311 закрыт, отписан; docs TASK_SYSTEM поправлен; хвосты → #326 |
| ✓ #328 R7a СЛИТ 42fd27a (23:30Z), refs #118 (открыт). СЛЕДУЮЩЕЕ: R7b (texture_id в texture_manager, пакеты, материалы, destroy(handle), sampler_handle; ниты: id драйвера в хендле, std::function на текстуру) → R7c |
| ✓ #330 R7b1 СЛИТ f021109 (02:26Z), refs #118. Следом R7b2 (texture_manager ids, материалы, пакеты) ∥ R7b3 (render_targets, capture) |
| ✓ #335 R7b2 СЛИТ 005bc08 (03:49Z), refs #118, отписан
| ✓ #337 desc-ext СЛИТ da395c9 (04:42Z), refs #131, отписан |
| ✓ #338 material СЛИТ 69bfca6 (05:08Z), refs #131, #321 закрыт, отписан; desc_resave 0 changed |
| ✓ #340 skybox СЛИТ 1948ba6 (05:35Z); #131 ЗАКРЫТ (EPIC-55: light/camera/material/skybox на схеме), отписан |
| ✓ #339 R7c СЛИТ ceabc2c (06:0xZ); #118 и #119 ЗАКРЫТЫ, отписан. Хвосты review: валидация region в update_texture, texture_interface::native() без внешних вызовов, texture_object_access в ctor validation вместо dynamic_cast |
| ✓ #336 checklist.png СЛИТ 9dbae26 (03:49Z), отписан; все эталоны теперь 0.000% |
| ✓ #334 R7b3 СЛИТ 1cabc01 (04:31Z), refs #118, отписан; все эталоны 0.000% |
| ✓ #329 камера СЛИТ a245b96 (02:59Z), refs #131 (2/3; material/skybox ждут решения владельца по core/core/desc) |
| ✓ #327 W5 СЛИТ 2b6a50c (23:30Z), #273 закрыт (мой фикс SNK_API f3645b5). Хвост EPIC-67: устаревший screen_anchor_result |
| ✓ #315 СЛИТ 314b05a (19:12Z), refs #131 (1/3, issue открыт), отписан |
| #131 material/skybox | a8156a5abaa0b869c (остановлен) | БЛОК: схема не выражает (1) запись null (`shader_geometry`, `queue` NONE), (2) read-only ключ `shininess` (баг потери данных → issue #321), (3) desc_ref с контекстом desc_system/owner (skybox inline/tag). Нужен PR-предпосылка в core/core/desc → ВОПРОС ВЛАДЕЛЬЦУ (дизайн-изменение). Ветки локальные, пустые, не запушены. camera — после W4 #319 |

## Очередь после текущих
- WORLD: W4 #272 (камеры/слои рендера) — после R6a #313 (extract); W5 #273 после W4.
- EDITOR: #140 PR-9 после #137.
- RHI: R6b #125 (material_instance + замер) после #313 → R7 #118/#229/#119; GLSL #122, #123.
- AI: A6a #292 (MCP read-only) после A3 #289; A4 #290, A5 #291 по плану.
- DESC: #131 camera, material после light; #314 (diff допуск float).
- После волны: #258 -Werror; хвосты #236, #277, #280, #283(п.6), #306, #307, #311, texture_manager-фасад (issue не заведён); ниты W2 (лишний <algorithm> в scn_context_set.cpp, комментарий 1/60 в scn_level_scheduler_tests.cpp:597).
- Проверить и закрыть по AC (похоже сделаны): #110, #119, #120, #135, #136.
- Вопросы владельцу: нет (2026-10-19: CLAUDE.md:62 заменён на `scn_context_set.cpp`; демо только финальное).
- Влито 2026-10-19 после компакта: #274 W1, #267, #275 R4, #279, #282 R5a, #281 PR-5, #285 A0, #295 smoke-gate, #297 A1, #303 hang-fix, #301 EDITOR PR-6. Закрыты: #269 #116 #287 #300 #138.

## #109 разделение заголовков (дизайн `docs/roadmap/design/HEADER_SPLIT.md`, утверждён, §6)
- PR-0 ✓ (#214): `snk_component_headers(<tgt>)` (include/ → PUBLIC, корень PRIVATE), install двух раскладок (`^core/.*/include$` → `include/`), `tools/ci/check_sdk_headers.sh` (report-only, 18 внутренних сегодня), INTERFACE `snk::scene_material_desc_api` (до PR-4) и `snk::window_gl_surface`, `.gitattributes` `*.sh eol=lf`.
- Инструмент ✓ (#215): `tools/header_split/` (`plan/apply/check/report`, components.json, DRY_RUN.md; private_access: render→rnd_gl_swapchain.h, gui_gl_backend→rnd_gl_texture.h, gui→gui_gl_backend.h; повышены до public: app_module_entry.h, ds_rtree.hpp, res_adapter_info.hpp, res_file_watcher.h, gui_profiler_overlay.h, scn_override_record.h, core_thread_utils.hpp).
- PR-1 ✓ (#228 → 97a329d): 16 заголовков, self-containment check в package_consumer (нашёл баг glm в inp_keyboard_event.hpp). Ниты в PR-5: убрать «Narrowed in PR-1 of #109» в window_service/CMakeLists.txt:10-11; `_sdk_prefix` в package_consumer предполагает lib/cmake/snake_sdk.
- PR-2 ✓ (#234 → 2945f1f): 50 заголовков, 332 строки (DRY_RUN.md устарел: 331); drv_opengl весь внутренний, render/gui_gl_backend видят весь его каталог PRIVATE (возможное усиление: friend/-подкаталог). Ниты PR-5: обновить DRY_RUN.md, пути в CODE_CONVENTIONS.toon:68, EPIC-11.md:34, ARCHITECTURE_REVIEW.md:90.
- PR-3 ✓ (#235 → 2071b14): 24 заголовка, 280 строк (= DRY_RUN), self-containment 90; фиксы ds_rtree.hpp, ds_svg_writer.hpp. В PR-5 доки: CLAUDE.md:30, CODE_CONVENTIONS.toon:107, CONSTRAINTS.toon:36,103 (`<snk/common/logger/engine_log.h>`), README.md:352, QUICK_LINKS.toon, BL-002, manifest.json, EPIC-23/31/37/39/43/44, SnkTarget.cmake:4 и ThirdParty.cmake (путь snk_api.h).
- PR-4 ✓ (#237 → 1ca8f66): 73 заголовка, 460 строк; check_sdk_headers enforce (0 internal), self-containment 163; Editor без include-корней core/engine; scene_material_desc_api оставлен → #236.
- PR-5 ✓ (#238 → 069cf34), #109 закрыт. Хвосты: #236, #239 (+SameMinorVersion).
- (история плана:) PR-5 → PR-4 engine-слой (+ убрать обходы, enforce проверки SDK, Editor core/engine include) → PR-5 финал (старая раскладка, SDK 0.2.0, доки, `.git-blame-ignore-revs` со squash-SHA, абсолютные пути glfw/assimp в SDK, CI-ниты из ревью #204). Dry-run: 16/50/24/73 заголовков, 75/331/280/459 строк include.

## Этап C ✓ по коду: #174 закрыт (#240 → f0046b5, 0 предупреждений GCC/Clang). -Werror: владелец 2026-10-19 — включить везде (CI и локально) → #258 после мержа #252/R3/#136; MSVC ~15 мест C4267. #55 — ручная проверка владельцем после волны; #57 — при мерже в main, список путей согласовать, #241 KP_DIVIDE.

## Этап D волна 1 — сделано: #233 S0 (#242), EPIC-28 #166–168 (#243), R0a (#244), #132 (#245), #128 (#246), R0b (#248), #129 (#250 → 57b607d). R0c (#249), #133 (#247), #127 R1 (#251 → 90b1f59: extract 0.250→0.083 мс), #110 PR-1 (#253 → fae48d2), R2 #115a (#254 → ca40103), #110 PR-2 (#255 → 15a43e8), EDITOR_FRAMEWORK PR-1 (#256 → d74b458), #134a (#252 → 717ad30), EDITOR PR-2 (#259 → d6638fc), #110/#111 PR-3 (#261 → 0acf56f), #134b (#262 → 5f1333c), #113 PR-4 (#264 → 1d2d278), #239 (#265 → e2f023e), детерминированные снимки (#260 → 88af3c4, diff 0.000%), RHI R3 (#257 → 93bd8b5), #112 PR-5 (#266 → 1b7c3dc). Дальше: после #247 → #134a; после #249 → #127 (R1, повтор бенча); D-трек #110 не параллелить с #127 (scn_render_data_extractor). #130/#131 — волна 2 (после #138).

## Этап D (план `docs/roadmap/STAGE_D_PLAN.md`, утверждён §5)
- Решения: context = один мир; код D — после PR-5 #109, затем сразу #174; null-бэкенд вперёд; #127 раньше POD; #114/#126 закрыты; Undo и Extension API в M3, job_service сначала над std::async; Boost.Describe; #123 отложен. Пути в #110–#140 поправлены.
- Утверждённые дизайны: `RHI.md` (R0 null → R1 #127 → R2–R7; решения: привязки по типам ресурса SNK_UBO/SSBO/TEX, камера в set 0, params-список, эталонные PNG в репо, перф-джоб отчётный, #119 разделён → #229), `WORLD_CONTEXT.md` (6 PR после #109; контексты целиком подряд, Play-набор, копия stepper, запрет межконтекстного доступа, нейтральный ввод, world→world_context, rename prefab_editor_context в #137).
- Волна 0 ✓ утверждена (25ebf74): TASK_SYSTEM (EPIC-28 в волну 1 до #134; #134a/b; US-17-5 hot reload по графу), EDITOR_FRAMEWORK (9 PR, job_service по форме TASK_SYSTEM), DESC_SCHEMA (S0 desc_resave --check до #128; light→camera→material; dyn_blob вариант A). Issues обновлены: #16–#18, #22, #24, #31, #128, #130–#139, #141; EPIC-28 #166–#168 → этап D; новые #232 (US-17-5), #233 (S0 desc_resave CI).

## Дистрибуция (эпик #217, дизайн `docs/roadmap/design/DISTRIBUTION.md`, утверждён)
- Обе модели (SDK-библиотека и game.dll-модуль редактора), DebugGame (/MD RelWithDebInfo движок), публикация только Windows, MSVC ≥ 14.44, NSIS, редактор shared-only, Stop→reload→Start, монолит отдельно.
- Stories: S1 #218 (spike EnTT/DLL, P0, M1), S2 #219, S3 #220, S4 #160 (перенесена), S5 #221, S6 #222, S7 #223, S8 #224, S9 #225, S10 #226, S11 #227 (монолит = путь Б), S12 #230 (spike wasm-слоя, P2, без вехи).
- §7: долгосрочно — wasm-слой игрового кода вместо LLVM (wasmtime/WAMR, wasi-sdk, WIT/C ABI, пакетный API, компоненты по схеме, id по имени, trap не роняет редактор); shipping: AOT по умолчанию, нативный монолит опционально. ADR-01 помечен (hot reload в объёме).

## Решения этой сессии (ранее)
- #10 шов: PLATFORM_SEAM.md, своя ImGui-платформа на событиях wnd — закрыт.
- #211 (EPIC-50 хвосты): фазы по id, сброс stepper, контракт reload (a): per-world экземпляры пересоздаются.
- MSVC shared: Boost.JSON/assimp статически в editor; imgui экспортируется из DLL.
- Кэш инспектора (#195): view_cache shared_ptr<const T> + desc_generation.

## Сделано на этапе C (в aibranch, issues закрыты)
- EPIC-29 #6, EPIC-50 #8, эпик #7 форматы, #9 цикл кадра, **эпик #10 шов** (#101–#104).
- 2026-10-18: #213 (#211), #210 (#207), #208 (#104), #204 (#201), #206 (#103), #205 (#202), #203 (#102), #190, #186, #192/#172, #195, #196, #198; #214 PR-0 и #215 инструмент для #109.
- Раньше: #176/#53, #54, #56, #108, #180; баги #81–#84 #169 #171 #173 #175 #177 #181 #182.

## Осталось по этапу C
1. #109 PR-1…PR-5 (последовательно).
2. #174 ~130 предупреждений — сразу после PR-5.
3. Хвосты: #209 клавиши/раскладки, #212 (уходит в R5 RHI), тест ключей кэша инспектора; #105 ждёт EPIC-34.
4. Ждут владельца: #55 ручная проверка Windows (shared DLL, Game view, HiDPI #207, браузер ассетов), #57 переписывание истории при мерже в main.
Потом этап D волна 1.

## Окружение
- Сборка локально медленная (4 ядра) — не собирать, CI. Логи CI: get_job_logs(return_content=false) → curl URL в scratchpad → grep.
- Headless: tools/run_headless.sh; tools/headless/check_smoke.sh.
- Коммиты: футер Co-Authored-By: Claude Opus 5.5 + Claude-Session. CRLF — Edit tool. Цепочки «проверка → push» только через `&&`.
- Лимиты Actions: репо публичный — минуты бесплатно; узкое место — 10 ГБ кэша (LRU 7 дней).
- 2026-10-19: дизайн WORLD_MESSAGING.md (#268, 4a1b6d8) — утверждён владельцем (2a22205, «да по всем шести»), #268 закрыт; issues W1–W5 #269–#273; W1 до PR-6.
- Наблюдение 2026-10-19: прогон 39207252577 (PR #267 @ d30a3eb) — 'Windows MSVC Shared Release' завис в 'Run unit tests' >18 мин (обычно ~12 с), причина не установлена; на ec994ce прошёл. Если повторится — расследовать (возможна редкая взаимоблокировка в тестах task_system/ресурсов, test_util deadline_guard).

## Обновление 2026-10-19 ~08:00Z
- #274 W1 влит squash d5f2d83, #269 закрыт.
- PR-6 #12: агент a0efe75706c86be57, wip/12-pr6-cleanup — пишет; после мержа #267 мержить aibranch.
- R4 PR #275 (a17971a13bd6d9a97): review APPROVE; доделки — serial в экспортируемый .cpp, m_bound=nullptr в begin_render_pass. Потом merge closes #116.
- #267 (a1de83c1418ec6119): фикс Esc у file_dialog в 0fd7498, ждём отчёт/CI.
- Нит-ы W1 в W2: fast path exchange() при 0 трафике; bind_missing только при смене числа типов; external-override без warn; warn при дропе недоставленного outbox; doc про два писателя в одном контексте.
- Новый issue #277 (scn::material_render_mode / wireframe).
- #267 влит squash fc1c465 (refs #139, issue открыт — дальше PR-4..). PR-6 агенту сказано смержить aibranch.
- EDITOR PR-4 job_service: агент ac4cdac85a1735024, wip/139-job-service. Решение: бэкенд сразу ds::task_system (есть в app_data_storage), API по решению 7 — отклонение от «сначала std::async», сказать владельцу.
- R4 #275 влит squash 6f6013e, #116 закрыт.
- R5 разбит: R5a (bind groups API, SNK_* макросы, set 0/1/3, ring UBO ObjectData, SSBO костей, MAX_LIGHT_COUNT) — агент a9a5e3ef4a8a79a9a, wip/117-bind-groups. R5b потом: MaterialParams/set 2, удалить uniform(), closes #121, #212 render_context.
- PR-4 job_service #279 влит squash c887b85 (refs #139). Хвосты → #280. Док API поправлен 72d7a46.
- EDITOR PR-5 #138 panel/action registry: агент ae689ec3a6d0aae7b, wip/138-panel-action-registry.
- AI_INTEGRATION: эпик #284, дизайн docs/roadmap/design/AI_INTEGRATION.md (8c48c9c) — ЖДЁТ УТВЕРЖДЕНИЯ, 6 вопросов §8. A0 (схемы+invoke(args)) отправлено в бриф PR-5 агенту ae689ec3a6d0aae7b (или отдельный PR wip/138-action-schemas).
- R5a #282: review APPROVE, хвосты → #283 (взять в R5b). Ждём CI → merge.
- R5a #282 влит squash 5e5ec46 (refs #117). R5b: агент a1d710bf9ccaf6ef7, wip/117-material-params — set 2 MaterialParams, удалить uniform() (closes #121), render_context (closes #212), хвосты #283, closes #117.
- PR-5 #281 влит squash 121253a (refs #138). #285 (A0 схемы) перенацелен на aibranch; CHANGES REQUESTED: отказ на неподдерж. ключевые слова схемы + minLength, action_result.code (needs_input...), file.open discard_unsaved; автор ae689ec3a6d0aae7b. Баг #286 (Save Level vs Ctrl+S).
- Следующее в EDITOR: PR-6 #138 inspector_registry/asset_kind_registry — после #285.
- AI_INTEGRATION #284 УТВЕРЖДЁН (решения 1–9): HTTP-only, сервер — STATIC модуль редактора без движка, игра через `editor --game`. Stories: A1 #287, A2 #288, A3 #289, A4 #290, A5 #291, A6a #292, A6b #293, A7 #294. Порядок: A1 после #285 → A2; A3 после R5b; A6a после A3; A4 после #137; A5 после #29.
- Владелец согласен: job_service на task_system, R4 invalidate в начале кадра, panel instances; visual smoke → БЛОКИРУЮЩИЙ (сделать маленький CI PR).
- #285 A0 влит squash 86cbd2b (refs #138, #284). A1 #287: агент ace96a94ec1171de9, wip/287-command-registry. EDITOR PR-6 #138 (inspector/asset_kind) — запустить, когда освободится место (после visual-smoke gate).
- #295 visual smoke теперь БЛОКИРУЮЩИЙ (squash 0b7280f).
- В работе (CI-only): PR-6 WORLD #12 (a0efe75706c86be57), R5b (a1d710bf9ccaf6ef7), A1 #287 (ace96a94ec1171de9), EDITOR PR-6 #138 (a214a56c8c4b5e761, wip/138-inspector-asset-registry), hot reload граф #263/#232 (aed0702f2b655fc50), #276+#286 save/close (a4c85ff00f29496d9), #209 клавиши (a866995af8a880763).
- Ждут: #258 -Werror (широкий, после волны), W2 после PR-6, R6 после R5b, A2 после A1, #130/#131 после EDITOR PR-6, #137 documents после EDITOR PR-6.
- ДЕМО: владелец хочет демо после этапа D на своём ПК через локальную сессию Claude Code. Документ docs/agent/handoff/DEMO.md (сценарий 15 шагов, сборка через Release workflow_dispatch → артефакт SnakeEditor-windows-x64, запасной путь dev-release, промпт для локального агента). Демо только финальное (решение владельца 2026-10-19). Перед демо: запустить Release workflow на aibranch и дать ссылку на артефакт.
- A1 #297 влит squash 21a4886, #287 закрыт. Агентам #138-inspector и #276-286 сказано смержить aibranch и адаптировать API (.value, cmd::command_desc*, cmd::json_schema).
- A2 #288: агент a1f623e45a39a369a, wip/288-log-jobs-dialogs (+ хвосты ревью A1: исключение в очереди, deadlock-гард, activate re-add).
- #296 R5b: CHANGES REQUESTED (группы материалов по содержимому), автор a1d710bf9ccaf6ef7. #303 hang-fix: ждём 2-3 зелёных Windows Shared Release. #297 смержен.
- #303 влит squash d61c2f6, #300 закрыт (1 зелёный Windows SR; фикс доказуемо убирает UB; таймаут 10 мин на шаге тестов страхует). Если зависание повторится — переоткрыть #300.
- EDITOR PR-6 #301 влит squash ec26392, #138 закрыт. A2-агенту сказано смержить aibranch (конфликт editor_context.h).
- #130 schema inspector: агент запущен, wip/130-schema-inspector.
- PR-6 WORLD #278: ревью идёт (a84a86388aee0ca06); автор должен смержить aibranch с фиксом #303 и перегнать CI. Новые issue: #306 (флаки OwnerDestroyedOnMain…), #307 (access_entry::world).
- #137 documents — запускать ПОСЛЕ мержа PR-6 WORLD (пересечение shared_state/level_controller).

## 2026-10-20 11:20Z
- Влиты хвосты #345 #346 #347 #348; закрыты #307 #306(dup) #326 #333 #343 #277 #236; открыты #349 #350 #351.
- Решения владельца: #322 → вариант B (editor_services у editor_system); wireframe → только редактор, debug-вид вьюпорта (#352).
- Запущены: #322 B (a18720bea0f05623f, wip/322-editor-services), #352 wireframe (ae4141047fbf53305, wip/352-editor-wireframe). Проверка CI aibranch после волны — send_later 12:00Z.

## 12:10Z
- #353 (#322 B) MERGED 1ff38a2, #322 closed. aibranch HEAD 1ff38a2.
- #354 (#352 wireframe) review OK, undrafted, full CI running; no overlap with #353. Smoke: capture 0.000%, editor shots 0.10–0.14% (new toolbar button) — references unchanged.
- #356 (#302 B alias edge + drop alias on signal) reviewed x2 OK, head 4b5ad24, fast tier running -> undraft.
- #357 benchmark GCC13 -Warray-bounds fix (tests/test_scene_env.hpp tag_of). Benchmark runs only on push to aibranch -> verify after merge.
- #105 agent still running.

## 13:30Z
- MERGED: #357 c908947 (benchmark fix; aibranch push 2f30d78: all 12 green incl. benchmark), #354 2f30d78 (#352 closed), #356 416cc10 (#302 closed; my fix c50dc18 = changed_batch tag by value, dangling ref).
- #355 (#105 assimp): reviewed OK; merged aibranch in (bcb0182) because res_system.h overlaps #356; full CI running. After merge: close #105, then check epic #11 ("public/ split" criterion).
- Next: tails #349 #350 #351 #280 #283p6; handoff update.

## 14:25Z
- MERGED #355 4116e16 (#105 closed). #11 epic: asked owner (public/ split criterion -> separate issue?).
- #358 (#350+#280.4): merged aibranch in (d06ac97, overlap edt_editor_system.cpp with #355), full CI running.
- #359 (#349): review OK + GL program cache reset (5d75b10), undrafted, full CI running.
- #360 (#351 erase): CHANGES — re-import data loss (erase only created tags), erase/store generation race, return false for absent. Author agent fixing.
- NEW SECURITY #361: vfs_resolver no sandbox containment (pre-existing write traversal from Assimp texture names; #360 erase would make it delete). Agent on wip/361-vfs-containment. #360 must NOT merge before #361; then route erase via safe_path.

## 2026-10-27 M1 kickoff (owner: "запускай все три, облачные агенты, отдельные сессии")
- session_01KQMEsfsk9Nas5u67XCwKPS — EPIC-36 edt:// #161–#165 (sonnet), branches wip/161..165-*
- session_01KQMEsjxYynvremtRtvW3ih — spike #218 EnTT+DLL, wip/218-dll-spike, PR "[spike, do not merge]", report docs/roadmap/design/SPIKE-218-entt-dll.md
- session_01KQMEsg4YXv7Uwp2sRV2xsx — EPIC-42 plan, wip/19-project-system-plan, docs/roadmap/design/EPIC-42-PLAN.md → owner confirm
- They report via GitHub issue comments; I review/merge PRs.
- In flight here: #377 (safe_path race, aibranch red), #378 (#368 camera, agent redoing matrix), #372 font code agent (wip/372-ui-font).
