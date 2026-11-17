# План этапа D («Фаза 0 — этап D», эпики #12–#18)

**Дата:** 2026-10-18 · **База:** `aibranch` @ 96b682c · **Режим:** read-only разведка (код не менялся). **Статус:** утверждён владельцем 2026-10-18 («да, по рекомендациям», см. §5). Источники: Issues #12–#18 + sub-issues (#110–#140), manifest.json, ARCHITECTURE_REVIEW §3.5–3.9/§7/§9, CHIMERA_ROADMAP ADR-04/07/08/10, HEADER_SPLIT.md, проверка кода (ссылки file:line ниже).
Размеры: S ≤ 3 дн, M ≈ 1–2 нед, L ≈ 2–4 нед, XL > 1 мес. Все 38 stories этапа открыты; прогресса по D в `git log` нет.

## 0. Сквозные наблюдения
- **Пути в stories устарели** (структура после EPIC-49/#176): `Engine/res/shaders/` → `core/res/shaders/`; `core/drivers/` → `core/core/video/drivers/{interface,impls/opengl}`; `core/core/ds/` → `core/core/common/ds/`; `core/engine/render/` → `core/engine/render/render_system/`. `drv_null/` не существует (создавать). Поправить тексты issues до старта.
- **Конфликт с #109** (HEADER_SPLIT §4, решение владельца §6.5): PR-0 инфраструктура сразу; PR-1…PR-5 строго последовательно, «без параллельных веток», каждый переписывает include во всём репо (~1100 строк в ~380 файлах). PR-1 application/core_input/window → PR-2 drv/ecs/desc/resource → PR-3 common → PR-4 input/gui/render/scene → PR-5 финал. D-эпики трогают ровно эти компоненты, поэтому **любая кодовая ветка D до PR-5 получит конфликты**; безопасна только документация.
- **Названия:** в редакторе уже есть `edt::prefab_editor_context` (`edt_prefab_editor_context.h`) — коллизия по смыслу с будущим `edt::editor_context` (#136); решить имена в дизайне #18.
- Нумерация US-15-x (#141–146, Undo, эпик #29) и EPIC-59 (#31) зависят от #137 (`document`) — #18 блокирует M3, см. §2.

## 1. По эпикам

### #12 world_context и context_manager — XL (L при условии разбора ниже)
Цель: `world_context` — единица симуляции (registry, фазы, время, ввод, command buffer, hot_reload), `level_manager` → `context_manager`.
| Story | Состояние | Зависимости |
|---|---|---|
| #110 world_context (структура) | open, нетронуто | #8 (done), **#211/PR #213** (правит `scn_level.h/.cpp` — мержить до) |
| #111 context_manager, level = набор контекстов | open | #110; удаляет `scn_level_manager.*` (54 упоминания `level_manager` в core/Editor/tests) |
| #112 превью префаба/сцены как контексты | open | #111; убирает «transient world» в `edt_prefab_editor_context.cpp:78`; **#186/#196** (done) частично пересекаются (`m_desc_world_names` в `level_manager`) |
| #113 per-context ввод | open | #110; `core/core/input/` — только события (`inp_*_event.hpp`), реальная маршрутизация в `core/engine/input/` (`inp_ecs_input_manager`), путь в story неточный; #209 (клавиши) касается тех же enum |
| #114 удалить соль/лимит 5/runtime_context_provider | **устарела** (см. §3) | — |
Дизайн-док: **нужен** (`docs/roadmap/design/WORLD_CONTEXT.md`); ARCHITECTURE_REVIEW §3.6 и ADR-07/08 дают направление, но не отвечают: (1) context = один `scn::world` или весь `level` (сейчас время, `fixed_stepper`, `mode`, seed, command-apply живут в `scn::level`, `scn_level.h:94-122`; `scn::world` — лишь registry+id+name `scn_world.h:11-32`); (2) судьба `LEVEL_ID`/`level_res<>` и `runtime_context.get_world_registry` (межмировой доступ, `ecs_invoker.hpp:24`); (3) единый command buffer (в `ecs_command_buffer.hpp` — sandbox-registry, в продакшене не используется) и порядок применения; (4) per-context ввод и как viewport выбирает контекст (мост к `inp_ecs_input_manager` и Game view #190); (5) hot reload per-context (флаг, а не `scn_hot_reload_manager` на level); (6) как render-экстракция (`scn_render_data_extractor`) и `camera_render_target` выбирают контексты; (7) сохранение уровня без контекстов превью; (8) стыковка с Play (EPIC-63/ADR-07: отдельный level vs отдельные контексты).
Риски: самый широкий по файлам эпик (scene/ecs/editor/tests); конфликт с #109 PR-4 и #211; баг D «мультимир» перепроверять после.

### #13 RHI шаги 1–4 + null-бэкенд — XL
Цель: убрать GL-понятия из абстракции (pass descriptors, неизменяемый pipeline, bind groups, typed handles) и дать бэкенд без GPU.
Цепочка строго последовательная: #115 → #116 → #117 → #118 → #119.
Состояние кода: `driver_interface` GL-стиля (`rnd_driver_interface.h:160-191`: push/pop FBO, `set_render_target`, `create_uniform_buffer(size, binding)`), `TEXTURE_USAGE` — одно значение, `has_usage` сравнивает ==, не флаги (`rnd_texture_interface.h:21,113`); GL вне `impls/opengl` не течёт (grep GL_/glad: только `GLFW_*` hint-и в `wnd_window_context.cpp:9-13` — для критерия «grep в CI» нужен allow-list). `rnd_swapchain_interface.h` и `gl::swapchain` уже есть (#102), `null_window_system` есть (#101) — основа для headless-рендера.
Дизайн-док: **нужен** (`design/RHI.md`, объём как PLATFORM_SEAM): §3.5 даёт лишь 4 строки на шаг. Вопросы: форма descriptors/load-store и кеш FBO в GL; usage-флаги и совместимость с `texture_manager`/`res::tag`; идентичность pipeline (`pipeline_desc` ↔ `shader_config`, #14/#15); раскладка set/slot (0 кадр, 1 пасс, 2 материал, 3 объект, §9.4) и per-draw UBO; handle: u32+generation, где живёт таблица, соотношение с ADR-11 (handle-индирекция текстур) и ImGui `ImTextureID`; контракт null/validation-бэкенда (что валидирует); порядок миграции пассов (6 пассов в `passes/`).
Совет: null-бэкенд на текущем интерфейсе — дешёвый (S–M) инструмент проверки, уже на шаге 1; в issue он последний (#119 ← #118) — **спросить владельца** (Q3).
Риски: самый большой риск этапа; затрагивает каждый пасс и `gui_gl_backend`; конфликт с #109 PR-2/PR-4, #212 (транзитивная публичность менеджеров через `rnd_render_system.h` — делать вместе с шагом 3/4, иначе дважды правим заголовки).

### #14 Правила шейдеров GLSL — M
Цель: единый include, `SNK_BINDING`, блоки без свободных uniform, явные ключи вариантов.
Цепочка: #120 → #121 → #123; #122 ← #120.
Состояние: 15 шейдеров в `core/res/shaders/`; `#version` 450 везде, кроме двух с 420; **свободных `uniform` всего 4** — `first_include.vert:2` (`uWorldMeshMatr`) и `phong_model_lib.frag:52-54` (shininess, diffuseColor, emissiveColor); сэмплеры уже с `layout(binding=)`. Включения — `#include "..."` текстом. Число света — define `DIRECTION_LIGHT_COUNT` (`rnd_material_compiler.cpp:38-39`) → новый вариант на каждое число (проблема из §3.5 жива). Хеш — XOR строк без имён (`rnd_scene_shader_desc.h:101-112`).
Дизайн-док: **достаточно** (§9.4 — 6 правил). Нужны мелкие решения: механика include-препроцессора (VFS-пути, guard'ы, кто владеет — `rnd_shader_manager`), как параметры материала попадут в блок, а не в `glUniform*` по имени (`rnd_gl_shader.cpp:17-33`) — **это зависит от RHI шага 3 (#117)**, поэтому #121 «завершить» можно только после #117; #123 (SPIRV-Reflect, glslang) — отдельная зависимость в CMake/CI, опционально.
Размер: #120 S, #122 S–M (независим от RHI), #121 M (из-за связи с материалами), #123 M. Эпик в целом M, но завершаем только после #13.

### #15 POD-пакеты рендера и материал-инстансы — L
Цель: тонкие draw packets + материал-инстанс, компилируемый при изменении (поглощает EPIC-45).
Stories: #124 → #125 → #127 (#126 — см. §3, устарела).
Состояние: EPIC-49 уже сделал POD `scn::material_desc` + render-side compiler (`rnd_material_compiler.h`, commit 3ed2cc9). `draw_call_t` (`rnd_render_packet.hpp:85-95`) всё ещё несёт `sort_key`, но и тяжёлый `shader_config material` и `const std::vector<glm::mat4>* bone_matrices`; камера хранит `res::tag` вместо id.
Дизайн-док: EPIC-45 (US-45-1…) **описывает кеш/handle**, но не POD-ids/offset костей; нужен короткий дизайн-раздел вместе с RHI.md (идентичность материал-инстанса = handle, где «компиляция при изменении» (сигнал hot reload `res`), буфер костей в кадровом аллокаторе). Порядок: EPIC-45 предлагает кеш (#127) **раньше** POD (#124) — в issues наоборот (#127 ← #125); **неопределённо**, согласовать (Q3).
Риски: зависит от typed handles (#118) для чистого результата; можно сделать промежуточный шаг (кеш → handle) без RHI и измерить ускорение (критерий эпика «измерено на сцене-образце»: цель EPIC-45 Extract < 2 мс/100 мешей).

### #16 EPIC-55 схема desc (DESC_FIELDS) — L
Цель: `type_schema` + generic serialize/deserialize/copy/diff/inspector + enum↔string; миграция desc по одному.
Цепочка: #128 → #129, #130 (параллельны) → #131 (← #129, #86 done).
Состояние: `desc_hints.hpp` — 3 структуры, ни одного использования (`grep hints::` пусто); Boost 1.84 берётся FetchContent, но `BOOST_INCLUDE_LIBRARIES json asio program_options test` (`cmake/ThirdParty.cmake:89`) — **нужно добавить `describe` (и `mp11`)**; в SDK/Windows shared это нужно проверить (header-only, но install-замыкание).
Дизайн-док: ADR-04 — **недостаточно для кода**; нужен `design/DESC_SCHEMA.md`: DESC_FIELDS vs чистый Boost.Describe + атрибуты; формат `type_schema` (имя, вид, умолчание, диапазон, enum, ref-тип, optional); совместимость с override-записями v3 (`scn_override_record`, json_pointer) и с `has_transform` (DATA_FORMATS_V2: сейчас presence-семантика, которую generic diff обязан сохранять); мост с рукописными `serialize/copy_to` (UB `static_cast`, §3.7); где реестр миграций (#131 vs `desc_migration_tests.cpp`, уже есть версии v3 — ADR-06); какие 3 desc переводить первыми (список «непереведённых» в описании эпика).
Размер: #128 M, #129 M, #130 M (editor), #131 L. Риски: затрагивает 16+ desc-типов; #130 пересекается с #138 (реестр инспекторов) и инспектором из #192/#195 — делать после/вместе с #18.

### #17 EPIC-64 task_system — L
Цель: пул + main-thread очередь, `task_handle` (прогресс/отмена/stop_token), lifetime-токены, загрузка ресурсов с графом зависимостей, импорт/экспорт как задачи.
Цепочка: #132 → #133 → #134; #135 ← #132 (реально ещё и ← #139 job_service).
Состояние: `task_system` отсутствует; есть `adapter_worker_base` (post/start/stop, `res_adapter_worker_base.hpp`) и `Boost::asio` в resource; **`require_sync` — 19 вызовов** (core/Editor/tests); импорт и экспорт редактора **уже асинхронны** (`std::future` + spinner: `edt_async_import_task.h`, `edt_asset_export_dialog.cpp:158`) — «UI не блокируется» (#135) частично выполнено, остаётся отмена/прогресс через handle.
Дизайн-док: ADR-10 — **почти достаточно** для #132/#133; для #134 нужен дизайн: как адаптер объявляет зависимости (граф, циклы, мульти-ресурсные, `res_control_block::then`), потокобезопасность (BL-006 сделана), где дренируется очередь (`app::loop_service`/frame stage — готово, #9), политика GL-ресурсов (создание только на главном потоке, ADR-11).
Размер: #132 M, #133 S–M, #134 L (рискованная: адаптеры, `res_system.h`, пересечение с US-28-1…3 #166–168 — составной ключ кеша в том же `res_system`), #135 S–M.
Риски: #134 конфликтует с EPIC-28 (#25); `core/core/ds/` в story неверен (нужен `core/core/common/ds/` либо `core/core/resource/`).

### #18 Каркас редактора — L
Цель: публичный `editor_context`, `document`/`document_manager`, публичные реестры, `dialog_service`/`job_service`, без friend/`init_helpers`.
Цепочка: #136 → {#137, #138, #139} → #140 (← #137, #138).
Состояние: `edt_init_helpers.cpp` 322 строки (20 `set_on_*` лямбд), `friend` в `edt_editor_system.h:41-44`, `edt_shared_state.h` 48 строк (14 файлов используют `shared_state`), `edt_panel_manager.h` приватный. Задача #140 по сути — финальная чистка после #136–#139.
Дизайн-док: **нужен** (`design/EDITOR_FRAMEWORK.md`): публичный API `editor_context` (сервисы, selection, шина команд — граница с Undo EPIC-15/#29), `document` vs `scene_document` (EPIC-59 #31 заявлен «частично покрыт» — зафиксировать границу), реестры (panel/inspector/asset_kind) vs EPIC-53 (#22) Extension API и ADR-01 (shared lib + `game_editor.dll`), владение мирами (`vector<world_entry>`), связь с #12 (документ = контекст?) и #17 (`job_service` на `task_system`; до его готовности — адаптер над `std::async`).
Размер: #136 M, #137 L, #138 M, #139 M, #140 S. Риски: самый «горизонтальный» эпик по Editor/*; #137 меняет `shared_state` везде (тесты `edt_*_tests`); #112 и #137 нужно согласовать (превью-документы).

## 2. Порядок и волны
Реальные зависимости между эпиками: #12 ⟷ #18 (документ ↔ контекст; #112/#137), #13 → #14(#121) и #15, #17 → #18(#139), #16(#130) ↔ #18(#138), #13/#15 ↔ #212. Независимы: #12, #13, #16, #17 между собой (кроме общих файлов).

**Волна 0 — сейчас, параллельно с #109 (только docs/read-only, нет конфликтов с include-rewrite):**
- Дизайн-доки (по образцу PLATFORM_SEAM/HEADER_SPLIT, с утверждением владельца): WORLD_CONTEXT, RHI (+POD-пакеты, шейдерные правила §9.4), DESC_SCHEMA, EDITOR_FRAMEWORK; для task_system — короткий addendum к ADR-10 (граф зависимостей #134). Порядок написания: RHI → WORLD_CONTEXT → EDITOR_FRAMEWORK → DESC_SCHEMA → TASK_SYSTEM; каждый док независим, можно разными агентами.
- Triage issues (закрыть #114, #126; поправить пути; см. §3).
- Мерж PR #213 (#211) — до PR-4 #109, и до любого кода #110.
- Прототипы вне репо/на эфемерных ветках без мержа допустимы, но выбрасываются.

**Волна 1 — после PR-5 #109 (и #174? — см. ниже), параллельно по непересекающимся файлам:**
- A: #17 (#132→#133, затем #134) — `common/ds`, `resource`.
- B: #14 #120 + #122 (шейдеры + `shader_manager`) → затем #13 шаги 1–2 (`drivers`, passes). #120/#122 и шаги RHI делят `rnd_shader_manager`/`rnd_gl_shader` — не параллелить с #116/#117.
- C: #16 #128→#129 (`core/desc`, новые файлы; без Editor).
- D: #12 #110→#111 (scene/level, ecs) — не параллелить с #112/#137 и с PR, правящими `scn_level.*`.
Параллельно без конфликтов: A + C + (B либо D). B и D оба трогают render-extraction (`scn_render_data_extractor`) — разносить по времени.

**Волна 2:** #13 шаги 3–4 + #14 #121 + #15 (#124,#125,#127) (совместно, одна ветка линий render); #12 #112/#113; #18 #136 → #137/#138/#139 → #140; #16 #130/#131 после #138.
**Волна 3:** #13 #119 (если не вынесен вперёд), #14 #123 (опционально), #135, закрытие эпиков, проверка бага D (мультимир).
**#174 (предупреждения):** запускать между волнами, когда нет открытых веток (правит много файлов); новый код D должен быть warning-clean.

## 3. Устаревшее / дублирующее / частично сделанное
- **#114 — устарела.** commit a960596 «remove type-inferred ordering and salted_type… registration_dispatcher (and its 5-world limit) and runtime_context_provider» (closes #90/#94/#79). `grep runtime_context_provider|salted_type|MAX_WORLD` по core/Editor/tests пуст. Остаток — только структура `ecs::runtime_context` (`ecs_invoker.hpp:24`), её судьба решается в #110/#111. Закрыть #114 со ссылкой на a960596; критерии эпика #12 «лимит 5 и соление удалены» уже выполнены.
- **#126 — устарела.** ADR-01 CHIMERA_ROADMAP.md:123: «`rnd::get_system()` из scene/пассов уже убран (US-49-6)»; `grep rnd::get_system` по `core/engine/render` — только комментарии (`rnd_frame_context.h:16`, `rnd_render_system.h:42`); `configure_pass(const frame_context&, T&)` (`rnd_render_system.h:74`). Закрыть со ссылкой на commit a2c4627. Критерий эпика #15 «пассы не используют `rnd::get_system`» выполнен.
- **#124 частично:** `sort_key` в `draw_call_t` уже есть (`rnd_render_packet.hpp:86`); осталось убрать `shader_config`/`bone_matrices*`.
- **#135 частично:** импорт/экспорт уже асинхронные (`std::future`), не блокируют UI; осталась отмена/прогресс на task_handle.
- **#121:** объём меньше описанного (4 свободных uniform в 2 файлах), но связан с RHI step 3.
- **Пересечение #137 ↔ #31 (EPIC-59 Document Model)** и **#138 ↔ #22 (EPIC-53)**: возможен дубль; разграничить в дизайне, иначе stories M3 переоткроют сделанное.
- **#112** частично покрывает #186 (resync миров после reload) — не дубль, но `m_desc_world_names` в `scn_level_manager.h` уйдёт вместе с `level_manager`.
- Формулировки эпиков: #13 «проверка grep в CI» — чек вне `drv_*` выполним уже сейчас (только GLFW hint-ы в window impl); #16 критерий «базовый набор desc» не определён числом (в #131 — «не менее трёх»).

## 4. Вопросы владельцу (≤7)
1. **Что такое context:** один мир (`scn::world`) со своими временем/вводом/командами, или группа миров с общим временем (нынешний `level`)? От ответа зависят #110/#111/#112 и связь с Play (ADR-07: «отдельный level»).
2. **Окно для кодовой работы D:** ждём PR-5 #109 целиком (рекомендация), или разрешаем аддитивные новые файлы (task_system, schema) между PR-1…PR-4 с ребейзом include-скриптом? Когда запускать #174 — до волны 1 или после?
3. **Порядок в RHI/пакетах:** выносить ли null/validation-бэкенд вперёд (на текущем интерфейсе, до шага 1) и делать ли кеш shader_config (#127/US-45-1) раньше POD-пакетов, как предлагает EPIC-45?
4. **Закрыть #114 и #126 как выполненные** (a960596, a2c4627) и править пути в stories — подтверждаете?
5. **Граница #18 с M3:** `scene_document`/`document_manager` (#137) и реестры (#138) входят в этап D целиком, а команды Undo (EPIC-15, #141–146) и Extension API (EPIC-53) остаются M3? Разрешено ли `job_service` сперва сделать над `std::async`, не дожидаясь #132?
6. **Boost.Describe для схемы (#128):** подтверждаем ADR-04 (Boost.Describe + таблица атрибутов; добавить `describe`, `mp11` в `BOOST_INCLUDE_LIBRARIES` и SDK-замыкание) или предпочитаете собственный макрос DESC_FIELDS без Boost-зависимости?
7. **Объём #14:** glslang/SPIRV-Reflect (#123) в этапе D или откладываем до не-GL бэкенда (§9.4 помечает «опционально»)?

## 5. Решения владельца (2026-10-18, «по рекомендациям»)
1. Context = один мир (`scn::world`) со своим временем, вводом и командами (как в формулировке #12); окончательно — в `WORLD_CONTEXT.md`.
2. Кодовая работа D — после PR-5 #109; #174 — сразу после PR-5, до волны 1.
3. Null-бэкенд выносится вперёд (на текущем интерфейсе); кеш `shader_config` (#127) — раньше POD-пакетов, как в EPIC-45.
4. #114 и #126 закрываются как выполненные (a960596, a2c4627); пути в stories поправить.
5. Undo (EPIC-15, #141–146) и Extension API (EPIC-53) остаются в M3; `job_service` сначала поверх `std::async`.
6. Схема desc — Boost.Describe по ADR-04 (`describe`, `mp11` в `BOOST_INCLUDE_LIBRARIES` и SDK-замыкание).
7. glslang/SPIRV-Reflect (#123) откладывается до не-GL бэкенда.
