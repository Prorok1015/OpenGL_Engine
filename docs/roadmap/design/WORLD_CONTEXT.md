# Дизайн: world_context и context_manager (эпик #12)

**Issue:** #12 (stories #110–#113; #114 закрыта — a960596) · **Решение владельца:** 2026-10-18 — контекст = **один** мир (`scn::world`) со своим временем, вводом и command buffer (STAGE_D_PLAN §5.1) · Основа: `ARCHITECTURE_REVIEW.md` §3.6, §9.5; `CHIMERA_ROADMAP.md` ADR-07, ADR-08; EPIC-63 (#39); стыковка с #18 (#136–#140) и #109 · **Статус:** черновик, ждёт утверждения (§6). Код — после PR-5 #109.

## 1. Как устроено сейчас (aibranch @ 9f993ec, после #211/PR #213)

- **`scn::world`** — только `entt::registry` + id + имя (`scn_world.h:11-32`). Всё остальное — в `scn::level`.
- **`scn::level`** (`scn_level.h:20-189`) владеет: мирами (`m_worlds` по имени + `m_worlds_by_id`, `:187-188`), общим временем (`m_stepper`, `:174`), режимом (`m_mode`, `:175`), seed (`:176`), списками систем и планами по world id (`m_world_systems`, `m_plans`, `:179-181`), порядком фаз и слотами (`m_phase_order`, `m_plan_slots`, `:185-186`), «level state» — отдельным registry (`m_level_state`, `:172`) и ссылкой на `hot_reload_manager` (`:171`).
- **Кадр** (`scn_level.cpp:32-74`): общий stepper → `delta_time`/`update_alpha`/`fixed_time` пишутся **в ctx level state** (`:53-54,61`); фазы идут по порядку, внутри фазы — все миры по id (`run_phase`, `:76-98`). После фазы command buffer применяется к каждому миру (`:94`) и к level state (`:97`). В конце — `hot_reload_manager->tick()` (`:71-73`). Step → Pause (`:69`).
- **RNG:** `ecs::rng` в ctx каждого мира, `derive_seed(m_seed, world_id)` (`:110`); у level state свой поток `derive_seed(m_seed, LEVEL_ID)` (`:15,21`). Пользователей у потока level нет.
- **Command buffer:** `ecs::entity_changer` (`ecs_entity_changer.hpp:40-66`) и sandbox `ecs::entity_spawner` (`ecs_command_buffer.hpp:15`) в ctx registry, применяются в `apply_commands` (`scn_level.cpp:248-275`). В продакшене никто не пишет — только тесты (`scn_level_scheduler_tests.cpp:607-631`).
- **Межмировой доступ:** `ecs::runtime_context{get_world_registry, current_registry, current_world_id}` (`ecs_invoker.hpp:24-29`). `bind_res<ID,T>` резолвится через `get_world_registry(ID)` (`:55-74`); level создаёт лямбду, где `LEVEL_ID` → level state (`scn_level.h:161-168`). `LEVEL_ID`/`level_res<>` — `scn_level.h:16-18`. Единственный продакшен-пользователь — `level_res<const delta_time>` (`scn_animation_job.cpp:35`); `fixed_time`/`update_alpha` (`scn_model.h:13-14`) никто не читает. Отсутствующий ресурс молча создаётся (`ecs_invoker.hpp:86`).
- **Владение:** `scn::level_manager` — один `active_lvl` (`scn_level_manager.h:46`), создаётся в `scn_scene_service_init.cpp:27-32`, тикает в стадии `update` (`gs_engine_frame_stages.cpp:14-16`). Hot reload уровня: `load()` подписывается на тег и сравнивает имена миров desc с `m_desc_world_names`; полная пересборка удаляет только desc-миры и одноимённые runtime-миры (`scn_level_manager.cpp:41-73`), иначе — только планы (`load_systems_from_desc`, контракт в `scn_level.h:130-136`). `set_on_reload` → редактор (`edt_init_helpers.cpp:267`).
- **Hot reload ассетов:** общий `hot_reload_manager` с набором registry (`attach/detach_registry`, `scn_hot_reload_manager.h:19-20,42`); level подключает **каждый** мир (`scn_level.cpp:112-114`), включая превью.
- **Редактор:** `shared_state` держит `weak_ptr<level_manager>` и массивы `world_descs/world_names/active_world_idx` (`edt_shared_state.h:17,29-34`); вся сцена — в режиме Edit (`edt_editor_system.cpp:82`). `world_controller::switch_to_world` ищет мир по имени, переносит editor camera (`edt_world_controller.cpp:23-63`); `create_world` создаёт runtime-мир прямо в level (`:86-141`). Превью префаба — «transient world» `__prefab_preview` **в том же level** (`edt_prefab_editor_context.cpp:78-91`, удаление `:105-115`): делит время, режим Edit, hot reload и порядок с уровнем. Сохранение строится из `world_descs`, не из level (`edt_level_controller.cpp:298,313`), поэтому превью не сохраняется уже сейчас.
- **Ввод:** один `inp::ecs_input_manager` (`inp_ecs_input_manager.h:14-38`) с одной `input_area`, общая система `inp::update_input_state` (shared-инстанс, `inp_ecs_input_manager.cpp:6-10`) копирует состояние в ctx **каждого** мира, где она есть в desc (во всех desc `Editor/sandbox`). Панели борются за `set_input_area` каждый кадр (viewport: `edt_viewport_panel.cpp:66-69,93-95`; префаб: `edt_prefab_editor_panel.cpp:81-90`). Фокус камеры — `inp::input_focus` в ctx мира (`inp_events.hpp:22`), панели пишут его вручную (`edt_viewport_panel.cpp:97-100`). Сброс дельт — `post_update` (`gs_engine_frame_stages.cpp:17-21`).
- **Рендер:** `render_data_extractor` берёт `level_manager` (`scn_render_data_extractor.h:7-12`) и обходит **все** миры и все камеры (`.cpp:21-25,40-42`); камера рисует в свои цели. В редакторе `camera_redirector` перед extract перенаправляет backbuffer-камеры в текстуры, scope = имя мира (`edt_camera_redirect.h:28-34`, `edt_editor_init.cpp:27-33`). Game view (#190) выбирает registry активного мира и обходит все (`edt_editor_system.cpp:132-145`).
- **Тесты:** `scn_level_scheduler_tests.cpp` (34 кейса), `scn_assembler_level_tests.cpp` (26), `scn_primitives_level_manager_tests.cpp` (20), `scn_level_find_world_tests.cpp`, `edt_empty_level_tests.cpp`, `edt_world_array_sync_tests.cpp`, `edt_game_view_tests.cpp`, `app_engine_without_editor_tests.cpp`, `inp_input_tests.cpp`; CI `editor-smoke` (`.github/workflows/build.yml:309-312`).

## 2. Решение

**`scn::world_context`** — единица симуляции (#110), новый файл `scn_world_context.{h,cpp}` (после #109 — в `include/snk/scene/level/`):

| Поле | Содержание | Откуда сейчас |
|---|---|---|
| `id` | `context_id{index, generation}` — уникален в `context_manager` | — |
| `stream` | номер мира в наборе (нынешний world id): seed, порядок | `world::m_id` |
| `name`, `kind` | имя; `kind ∈ {level, runtime, preview, play}` | `world::m_name`; origin в `m_desc_world_names` |
| `registry` | `entt::registry`; ctx содержит `delta_time`, `update_alpha`, `fixed_time`, `ecs::rng`, `inp::input_state`, `inp::input_focus`, command buffer | `world::m_state` + level state |
| `systems`, `plan`, `slots` | список систем, `ecs::system_plan`, слоты по `phase_order` | `m_world_systems`, `m_plans`, `m_plan_slots` |
| `stepper`, `mode` | `ecs::fixed_stepper`, `ecs::run_mode` | общие в level |
| `hot_reload`, `extract` | флаги: подключён ли registry к hot reload; отдаёт ли камеры в рендер | неявно «всегда» |

`world_context::update(dt, phase_order)` — тело нынешних `level::update` и `run_phase` для одного мира: stepper → время в **свой** ctx → фазы → после каждой фазы `apply_commands(registry)`. `scn::world` на время миграции — алиас (`using world = world_context`), чтобы `state()/get_id()/get_name()` у вызывающих не менялись.

**`scn::context_manager`** (#111) заменяет `level_manager`. Владеет контекстами (стабильные адреса, `unique_ptr`) и **наборами** `context_set{name, tag, seed, contexts[], active}`. Уровень = набор `"level"`: `load_level(tag)` создаёт по контексту на `world_desc`; hot reload уровня сравнивает только контексты `kind==level` этого набора (заменяет `m_desc_world_names`). API: `create_context(set, name, kind, systems)`, `destroy_context`, `find(set, name)`, `for_each(set|all)`, `set_mode(set, mode)`, `set_seed(set, seed)`, `update(dt)`, `set_on_reload`. `scn::level` остаётся тонким фасадом над набором `"level"` до PR-6.

**Общее для всех контекстов:** `res::resource_system`, `rnd::render_system`, `ecs::system_factory` (реестр систем и **таблица фаз** — один `phase_order` у менеджера), `scn::ecs_assembler`, сервис `hot_reload_manager` (наблюдение за ресурсами), сырой источник ввода. **Своё у контекста:** всё из таблицы выше.

**Следствия решения «контекст = один мир»:**
- *Время.* Каждый контекст шагает сам; менеджер подаёт один `dt` всем. Stepper детерминирован (`ecs_fixed_stepper.hpp:6-12`), сбрасывается при загрузке и clear у всех контекстов набора одновременно → число шагов совпадает с нынешним. Контекст, созданный в существующий набор, **копирует** состояние stepper первого контекста набора (lockstep, Q3).
- *Режим.* Хранится в контексте; `set_mode(set, …)` выставляет его всем контекстам набора. Step: каждый контекст делает ровно один кадр и уходит в Pause, как сейчас.
- *RNG.* `derive_seed(set.seed, stream)` — те же потоки, что сейчас. Поток `LEVEL_ID` исчезает (пользователей нет); тест `Rng_*` сохраняется.
- *`LEVEL_ID`/`level_res`.* Переходно `LEVEL_ID` резолвится в **свой** registry (время уже там). Финально системы берут `const scn::delta_time&` напрямую; `level_res`, `LEVEL_ID` удаляются. Отсутствующий ресурс → ASSERT вместо молчаливого `emplace` (`ecs_invoker.hpp:86`), время пишется до первой фазы.
- *Межконтекстный доступ (политика).* Системы видят **только свой** registry: `runtime_context{registry&, context_id}`; `get_world_registry` и `bind_res<ID>` к чужим мирам удаляются (в продакшене не используются). Обмен между контекстами — только вне фаз: редактор, стадии кадра, `play_session` (через `context_manager`). Канал сообщений — только при появлении реального сценария (Q4).
- *Command buffer.* Свой у контекста (ctx registry, как сейчас), применяется после каждой фазы **этого** контекста. Записи извне (редактор) применяются на ближайшей границе фаз контекста — как сейчас. Level state и его apply исчезают.
- *Порядок.* Наборы — в порядке создания (`level` → `preview:*` → `play`), внутри набора — по `stream`, каждый контекст проходит **все свои фазы целиком** (Q1). Неактивный набор не обновляется. `hot_reload_manager->tick()` — один раз в конце `context_manager::update` (позже — своя стадия кадра, §3.6).

**Hot reload per context.** Флаг `hot_reload`: при `true` registry подключён (`attach_registry`), при `false` — нет. Умолчания: `level`/`runtime`/`preview` — вкл, `play` — выкл. Наблюдение за desc уровня — у набора `level` в `context_manager`; runtime-контексты переживают reload (как #186/#196). Контракт перезагрузки систем (`scn_level.h:130-136`) переносится на контекст без изменений.

**Рендер.** Экстрактор берёт `context_manager` и обходит контексты с `extract==true`; выбор камер и целей не меняется. `camera_redirector::redirect(reg, scope)` получает scope `set/name` (иначе одноимённые миры превью и уровня столкнутся). Game view получает активный контекст и набор. В Play набор `level` получает `extract=false` (Q2).

**Ввод по контекстам (#113).** `ecs_input_manager` остаётся единственным приёмником событий окна (сырое состояние, без `input_area`). Новый `inp::input_router` (`core/engine/input/`) хранит привязки `bind(context_id | set, rect)`. Панели вызывают его каждый кадр: viewport → активный контекст набора `level` (или play-двойник), окно префаба → контекст превью, Game view → активный контекст `play`/`level`. Перед стадией `update` роутер пишет `inp::input_state` в ctx: контекст под курсором с фокусом получает полное состояние, остальные — нейтральное (кнопки отпущены, дельты 0, последняя позиция; Q5). `input_focus` выводится из привязки, ручные `ctx().emplace<input_focus>` в панелях удаляются. Без редактора привязка по умолчанию — весь backbuffer → набор `level` (все контексты, как сейчас). Система `inp::update_input_state` сохраняет имя (оно есть в desc) и читает состояние, подготовленное роутером.

**Play (ADR-07, EPIC-63).** «Отдельный `scn::level`» из ADR-07 = отдельный **набор** `play`: `play_session` строит его из копий desc документов (не файлов), со своим seed, `mode=play`, `hot_reload=false`, `user://` → тестовый профиль. Stop = `destroy_set("play")`. Набор `level` на время Play неактивен (Q2). Запись в авторский контент невозможна, потому что контексты не связаны с документами.

**Превью как контексты (#112).** Превью префаба — набор `preview:<tag>` с одним контекстом `kind=preview` (своё время, режим Edit, hot reload вкл, своя привязка ввода). Transient world в уровне (`edt_prefab_editor_context.cpp:78-91`) и коллизия имён при reload уровня (`scn_level_manager.cpp:56-62`) уходят. Превью сцены и ассета — тот же механизм.

**Сохранение.** Источник сохранения — документ (`world_descs`), не контексты; уже так (`edt_level_controller.cpp:298,313`). Добавляется тест: при открытом превью файл уровня не содержит `__prefab_preview`, а reload уровня не трогает набор превью.

**Граница с #18.** #12 даёт движковое API (наборы, контексты, привязки ввода), #18 решает владение: `scene_document` владеет набором `level`, документ префаба — набором `preview:*`; Play — сессия при `scene_document`, не документ. `edt::prefab_editor_context` переименовать в #137 (`prefab_document`), чтобы «context» не значил три вещи (`world_context`, `editor_context`, превью). `shared_state.lvl_manager` → `weak_ptr<context_manager>` в PR-3; имена миров в `world_names` остаются ключом поиска в наборе до #137.

## 3. Решения (рекомендации, ждут подтверждения)

1. Контекст = один мир; `scn::world` сливается с `world_context` (алиас до PR-6).
2. `level_manager` → `context_manager`; уровень, превью и Play — наборы контекстов; `scn::level` — фасад до PR-6.
3. Свои у контекста: время, режим, RNG (те же seed-потоки), ввод, command buffer, флаги `hot_reload`/`extract`. Общие: ресурсы, рендер, таблица фаз, фабрика систем, сервис hot reload.
4. Системы не видят чужие контексты; `LEVEL_ID`, `level_res`, `get_world_registry`, межмировой `bind_res` удаляются.
5. Порядок: наборы по созданию, контексты по `stream`, все фазы одного контекста подряд.
6. Ввод — через `input_router` с привязками viewport → контекст; без редактора — всё в набор `level`.

## 4. Шаги (каждый — отдельный PR, после PR-5 #109; CI: Linux gcc/clang static+shared, Windows MSVC shared, `unit_tests`, editor-smoke без изменений)

1. **PR-1 (#110, структура).** `world_context` + юнит-тесты создания/уничтожения/кадра. `scn::level` внутри хранит контексты вместо `world`+карт; stepper и режим держатся одинаковыми во всех контекстах; `LEVEL_ID` → level state по-прежнему. Поведение не меняется: 34 теста планировщика зелёные **без правок**.
2. **PR-2 (#110, время).** Время пишется в ctx каждого контекста; `LEVEL_ID` резолвится в свой registry; `scn_animation_job.cpp:35` → `const scn::delta_time&`; level state и его apply удаляются (переписываются `LevelState_GetsItsCommandsApplied`, `sys_capture_dt`). ASSERT на отсутствующий ресурс времени.
3. **PR-3 (#111).** `context_manager` + `context_set`; `scn_level_manager.*` удаляются; правятся service init, стадия `update`, экстрактор, `camera_redirector`, `shared_state`, `world_controller`, `level_controller`, тесты (`scn_primitives_level_manager_tests` → `scn_context_manager_tests`, `app_engine_without_editor_tests`, `edt_*`). Hot reload уровня — только набор `level`.
4. **PR-4 (#113).** `input_router`, привязки из viewport/префаба/Game view; `input_focus` из привязки; тест «ввод viewport-а префаба не влияет на основной».
5. **PR-5 (#112).** Превью префаба — набор `preview:<tag>`; тест сохранения без превью и reload уровня при открытом превью.
6. **PR-6 (чистка).** Удалить `LEVEL_ID`, `level_res`, межмировой `bind_res`, `get_world_registry`, фасад `scn::level` (вызовы → `context_manager`/набор), алиас `world`; поправить `ecs_invoker_tests`, `ecs_system_*_tests`, ARCHITECTURE.toon/доки; перепроверить баг D «мультимир».
Затем EPIC-63 строит `play_session` на наборах. #18 (#137) — после PR-5, чтобы документы сразу владели наборами.

## 5. Риски

- **Расхождение времени** между контекстами набора при создании в рантайме (решение — копировать stepper; в продакшене нет систем `fixed_update`, риск низкий).
- **Молчаливые ресурсы:** переход с `level_res` на `const T&` без ASSERT даст тихий 0 (`ecs_invoker.hpp:86`).
- **Порядок:** при «все фазы контекста подряд» меняется только видимость межмировых команд (сейчас не используется); тест `Worlds_RunInIdOrderEveryPhase` проходит при обоих порядках.
- **Ширина правок:** ~54 упоминания `level_manager` (core/Editor/tests), конфликт с #109 PR-4 (scene) и #137 (`shared_state`); не параллелить с #112/#137 и с правками `scn_level.*`.
- **Имена как ключи:** редактор ищет миры по строкам; при одноимённых мирах в разных наборах нужен ключ `set/name`.
- **Рендер/ввод:** экстракция и редирект камер завязаны на «все миры» — пропуск флага `extract` даёт двойную отрисовку в Play.

## 6. Вопросы владельцу

1. Порядок: контексты целиком друг за другом (рек.) или чередование «фаза × все миры», как сейчас?
2. Play: набор `level` не обновляется и не экстрагируется; Scene viewport показывает play-двойник активного мира с editor camera (рек.) — или edit-сцена продолжает тикать в Edit?
3. Контекст, созданный в рантайме в существующий набор, копирует stepper набора (рек.) или стартует с нуля?
4. Полный запрет межконтекстного доступа из систем (рек.) или read-only доступ внутри набора?
5. Неактивные по вводу контексты получают нейтральное состояние (рек.) или замороженное последнее?
6. Имена: `scn::world` сливается в `scn::world_context`, набор — `context_set`, `edt::prefab_editor_context` переименовывается в #137 (рек.)?
