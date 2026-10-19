# Дизайн: каркас редактора — editor_context, документы, реестры, сервисы (эпик #18)

**Issue:** #18 (stories #136–#140) · **Статус:** утверждён владельцем 2026-10-18 (решения в конце) · Основа: `STAGE_D_PLAN.md` §1 #18, §5 п.5 (Undo и Extension API остаются в M3; `job_service` сначала поверх `std::async`); `WORLD_CONTEXT.md` §2 «Граница с #18», §4 (утверждено); `DISTRIBUTION.md` §2 (утверждено: `registration_context`, области, `game_editor.dll` → #22, problems → #24, сборка через `job_service`); `CHIMERA_ROADMAP.md` ADR-01, ADR-05, ADR-10; стыковка с M3: #31 (EPIC-59), #22 (EPIC-53), #29/#141–#146 (EPIC-15), #24 (EPIC-51), #17/#135

## 1. Как устроено сейчас (aibranch)

**Владение.** `ds::app_data_storage` → `edt::editor_system` (`edt_editor_init.cpp:19`). Он владеет всем, и всё это приватно (`edt_editor_system.h:60-122`):
- `shared_state m_state` (`:62`), контроллеры `scene_editor`/`world_controller`/`level_controller` (`:65-67`);
- ввод `input`/`ecs_input` (`:76-77`), `file_dialog` (`:78`), импорт и экспорт (`:81-85`);
- `editor_layer` (внутри него `panel_manager` и `dockspace`, `edt_editor_layer.h:30-32`) и `component_ui_registry` (`:88-89`);
- семь панелей как `shared_ptr` (`:91-97`), `prefab_editor_context` (`:100`), состояние editor camera (`:105`), три ad-hoc асинхронных автомата (`:84,110-118`).

Сервисы движка приходят ссылками в конструктор (`:49`). `level_manager`, `system_factory` и `ecs_assembler` берутся из storage в `init` и кладутся в `shared_state` (`edt_editor_system.cpp:80-84`).

**`shared_state`** (`edt_shared_state.h:13-47`) — это и документ, и шина. В нём:
- ссылки на сервисы (`:17-19`);
- идентичность уровня: `editor_tag`, `level_tag`, `level_name`, `level_guid`, `is_dirty` (`:22-27`);
- три параллельных массива миров и `active_world_idx` (`:30-34`); их выравнивает `derive_world_arrays` (`edt_level_controller.h:30-44`);
- `desc_generation` (`:36`), на нём держится `override_edit::view_cache` (#192/#195, `edt_override_edit.h:45,63`).

Пользователи: 13 файлов `Editor/code/editor_system/controller/*` и тесты `scn_node_id_tests.cpp:248,363,608`, `edt_empty_level_tests.cpp:7`.

**Как панели получают состояние.** Только через сеттеры и лямбды, которые собирает `init_helpers`:
- `set_registry_getter` у viewport (`edt_init_helpers.cpp:76-84`) и у Game view (`edt_editor_system.cpp:133-145`) ищет мир по имени через `lvl_manager.lock()`;
- инспектор получает сырой `prefab_node*` (`edt_inspector_panel.h:23,48`).

Панели не знают ни о документе, ни о сервисах.

**Сеть колбэков** (`edt_init_helpers.cpp`, 322 строки: 20 `set_on_*` и 9 присваиваний `on_* =`):
- hierarchy → inspector/viewport/scene_editor (`:119-166`);
- меню dockspace (`:169-225`);
- контроллеры → панели (`:238-271`), в том числе `level_manager::set_on_reload` (`:267`).

Две функции объявлены `friend` (`edt_editor_system.h:41-47`), потому что лезут в приваты:
- drop ассета во viewport трогает `sys.m_state`, `m_pending_desc_*`, `m_scene_editor`, `editor_layer`, `poll_pending_desc` (`:87-96`);
- камера — `sys.save_editor_camera_state`/`inject_editor_camera` (`:250-254`).

Подтверждение сохранения держится на публичном поле `level_controller::m_confirm_action` (`edt_level_controller.h:117`, `edt_init_helpers.cpp:180,217`).

**Выделение** живёт внутри `scene_hierarchy_panel`. Это сырые указатели в desc (`m_selected_node`, `m_selected_owner`, `edt_scene_hierarchy_panel.h:64-70`) и путь из node id (`:68`). Как оно расходится:
- инспектору выделение передаёт лямбда (`edt_init_helpers.cpp:119-125`);
- viewport получает `entt::entity`, найденный по пути (`:99-109`);
- перед каждой правкой выделение сбрасывается целиком (`on_before_modify`, `:238-241`), потому что указатели протухают.

**Правки.** Каждая правка делает одно и то же: меняет `world_descs`, ставит `is_dirty = true` (8 мест в `edt_scene_editor.cpp:41…216`, плюс `edt_world_controller.cpp:139`, `edt_level_controller.cpp:85`, `edt_init_helpers.cpp:145,159`) и вызывает `serialize_and_push()`. Единой точки входа нет, так что Undo встроить некуда.

**Меню и панели** зашиты в код:
- File-меню и Ctrl+S — `edt_dockspace.cpp:52-69`;
- раскладка по заголовкам окон — `:100-112`;
- `panel_manager` — просто вектор `add/remove/find` по id (`edt_panel_manager.cpp:7-41`), из редактора не виден;
- реестр инспекторов по `__type` есть (`edt_component_ui_registry.h:14-40`), но без удаления и спрятан в `editor_system`;
- вид ассета зашит: `.prefab.desc` → превью (`edt_editor_system.cpp:119-125`).

**Диалоги.**
- Все модальные окна идут через `register_implicit` (`gui_menu_layout.cpp:77-90`, колбэк зовётся каждый кадр, пока возвращает `true`). Вызовов 9 (`edt_init_helpers.cpp:95,181,193,196,202,213,218,270`; `edt_editor_system.cpp:164,182`).
- Один общий `file_dialog` перенастраивается перед каждым вызовом (`edt_init_helpers.cpp:189-213`).
- Модалки разбросаны: `edt_level_controller.cpp:103-109,168-171`, `edt_editor_system.cpp:215,310,343`, `edt_asset_export_dialog.cpp:44,70`.

**Фоновые задачи.**
- Импорт: `std::async` с захватом `this` (`edt_editor_system.cpp:198-203`), опрос и спиннер (`:209-254`), токен `async_import_token` (`edt_async_import_task.h:29-33`).
- Экспорт: `std::async` (`edt_asset_export_dialog.cpp:158-162`) с атомарным `export_progress` (`edt_asset_exporter.h:13-19`).
- Загрузка уровня и ассета — это опрос `res_handle` (`edt_editor_system.cpp:296-332,334-364`).
- Отмены нет нигде. При выходе деструктор `std::future` блокирует поток.
- `require_sync` на главном потоке: `edt_level_controller.cpp:72,235,281,340`, `edt_prefab_editor_context.cpp:42,50,72`, `edt_world_controller.cpp:99,121`.

**Превью префаба** `prefab_editor_context` — read-only. Это мир `__prefab_preview` в общем level (`edt_prefab_editor_context.cpp:81-88,112-113`); `is_dirty` есть, а сохранения нет. Его уберёт #12 PR-5 (#112).

**Тесты:** `edt_panel_tests` (panel_manager, hierarchy), `edt_inspector_commit_tests` (в том числе `register_implicit`), `edt_empty_level_tests`, `edt_world_array_sync_tests`, `edt_game_view_tests`, `edt_inspector_console_tests`, `edt_light_inspector_tests`, `edt_asset_layers_tests`, `scn_node_id_tests`; CI `editor-smoke` (Xvfb, `levels/smoke_level.desc`). Путь `editor_system::init` покрыт только smoke.

## 2. Решение

Все новые типы — в `namespace edt`. Публичные заголовки лежат в `Editor/code/editor_system/include/snk/editor/` (раскладка как в #109); устанавливать их будет #22 (Q1). Термины фиксируются так:
- **context** — только `scn::world_context` (#12);
- **editor_context** — фасад редактора;
- **action** — команда меню или горячей клавиши;
- **command** — отменяемая правка, появится в M3 (#141).

**`editor_context`** (#136). Принадлежит `editor_system`, кладётся в `app_data_storage` (`data.require<edt::editor_context>()`). Через этот объект #22 потом выдаст доступ `game_editor.dll`.
```cpp
class editor_context {
public: // движок (не владеет)
	res::resource_system& resources(); desc::desc_system& descs(); rnd::render_system& render();
	gui::gui_system& gui(); scn::context_manager& contexts(); scn::ecs_assembler& assembler(); ecs::system_factory& systems();
	// редактор (владеет)
	document_manager& documents(); selection_service& selection(); editor_events& events();
	panel_registry& panels(); action_registry& actions(); inspector_registry& inspectors(); asset_kind_registry& asset_kinds();
	dialog_service& dialogs(); job_service& jobs(); editor_input& input(); // input = `inp::input_router` + привязки панелей (#113)
};
```
- Панель получает `editor_context&` в фабрике. Сеттеры-лямбды заменяются подписками.
- `editor_events` — типизированные сигналы с RAII-подпиской (`subscription`): `document_opened/closed/activated`, `document_changed(doc, generation)`, `active_world_changed(doc)`, `selection_changed`, `level_reloaded`.

**Выделение** (`selection_service`) — по стабильным id (ADR-05). В нём хранится `{document_id, world (имя в наборе), node path: vector<uint64_t>, readonly}`; узел ищут через `scene_editor::resolve_node_path` (`edt_scene_editor.h:53`) при каждом обращении. Сырых указателей в сервисе нет. Пока идёт этап D поведение не меняется: `document_changed` и смена мира сбрасывают выделение, как `:238-241,255-261` (сохранять выделение через правки будут EPIC-61/#33).

**Правки — одна точка входа (граница с Undo).** `scene_document::commit(std::string_view label)` выполняет `is_dirty = true`, `touch_descs()`, `serialize_and_push()` и `events.document_changed`. Все 12 мест из §1 зовут `commit`. Интерфейс `edt::command`, стек и save-point в #18 не вводятся (Q2). Что из этого получает M3:
- #141 оборачивает `commit` в `command_manager` документа;
- `label` превращается в текст меню Edit (#145);
- #144 заменяет флаг dirty на save-point.

**Документы** (#137, после #12 PR-5).
```cpp
class document { public: document_id id() const; virtual std::string_view kind() const = 0; const std::string& title() const;
	const res::tag& source() const; bool is_dirty() const; uint64_t generation() const;
	virtual bool can_save() const; virtual save_result save(); virtual save_result save_as(const res::tag&); virtual ~document(); };
class document_manager { public: scene_document& scene(); document* active(); void set_active(document_id);
	prefab_document& open_prefab(const res::tag&); void close(document_id); void for_each(fn); }; // в D: одна сцена + ≤1 превью
```
- **`scene_document`** = `shared_state` минус сервисы:
  - идентичность уровня (`editor_tag`, `level_tag`, имя, `__guid`);
  - `std::vector<world_entry{desc, name, systems}>` вместо трёх параллельных массивов, плюс `active_world`;
  - `desc_generation`;
  - **набор контекстов `level`** (`context_set&` из #12 PR-3); `world_entry::name` остаётся ключом в наборе (`WORLD_CONTEXT` §2);
  - состояние editor camera (`edt_editor_system.h:105`).

  `scene_editor`, `world_controller` и `level_controller` получают `scene_document&` вместо `shared_state&`. `view_cache` держит `const scene_document&`. Сохранение по-прежнему строится из `world_entry::desc`, а не из контекстов.
- **`prefab_document`** = переименованный `prefab_editor_context` (`WORLD_CONTEXT` §6, решение 6). Владеет набором `preview:<tag>`. `can_save()=false`, пока EPIC-25 (#30) не сделает его редактируемым.
- Play — это сессия при `scene_document` (EPIC-63), а не документ.

**Реестры** (#138). Это in-process реестры с ключом-строкой и операциями add/remove, ничего не знающие о DLL (`DISTRIBUTION.md` §2). `add` возвращает RAII-`registration` (move-only, снимает регистрацию в деструкторе). Встроенные панели и пункты меню проходят через тот же API.
| Реестр | Ключ → значение | Заменяет |
|---|---|---|
| `panel_registry` | `id` → `{factory(editor_context&), title, dock_slot, open_by_default}` | `panel_manager` (становится реализацией), заголовки в `edt_dockspace.cpp:100-112` → `dock_slot{left,right,center,bottom_left,bottom_right}` |
| `action_registry` | `id` (`"file.save"`) → `{menu_path, shortcut, run, enabled}` | File-меню и Ctrl+S (`edt_dockspace.cpp:52-69`), `dockspace::set_on_*` |
| `inspector_registry` | `desc __type` → renderer (сейчас), schema-fallback (#130) | `component_ui_registry` (+ remove, + доступ из `editor_context`) |
| `asset_kind_registry` | суффикс/тип → `{open(ctx, tag), drop_into_scene, icon}` | `.prefab.desc` (`edt_editor_system.cpp:119-125`), drop во viewport (`edt_init_helpers.cpp:87-96`) |

**`dialog_service`** (#139) построен поверх `register_implicit`, ImGui-контекст не меняется. Модалки идут очередью, одна за раз, с уникальными id.
- API: `confirm_unsaved(doc, then)` (Save / Don't save / Cancel), `pick_file(open_options, on_pick)` — у каждого вызова свои фильтры, общий `file_dialog` не мутируется; `save_as(doc)`, `message(level, text)`, `progress(job)`.
- Модалки `level_controller` и загрузки (`:103-171`, `edt_editor_system.cpp:215-364`) переезжают сюда, `m_confirm_action` удаляется.
- Тесты подменяют ответы (`dialog_service::set_auto_answer`), без ImGui.

**`job_service`** (#139) над `std::async`. Позже бэкенд меняется на `ds::task_system` (#132/#135), API при этом не трогается (ADR-10).

> Отклонение: backend = ds::task_system сразу (#132 уже есть), API по решению 7.

```cpp
struct job_context { std::stop_token stop; void report(float fraction, std::string_view note = {}); };
template<class R> job<R> run(std::string label, std::function<R(job_context&)> work, std::function<void(R&&)> on_done, std::weak_ptr<void> owner);
job<void> watch(res::res_handle<T>, std::function<void(res::res_handle<T>&)> on_ready); // загрузка уровня/ассета
void poll();          // раз в кадр из editor_layer::on_update; on_done — только на главном потоке и при живом owner
void cancel_all_and_wait(); // в ~editor_system
```
`job` — это `{status, progress, error, cancel()}`. Отмена кооперативная. Импорт (assimp) прервать нельзя: при отмене результат выбрасывается. Результат несёт `diagnostics` — их потом читают problems-панель (#24) и сборка `game.dll` (`DISTRIBUTION.md` §2). Импорт, экспорт и два опроса `res_handle` переходят на `run`/`watch`, а модальный прогресс — на `dialog_service::progress`.

**Удаляются** (#140): `edt_init_helpers.*` (функции камеры уезжают в `edt_editor_camera.cpp`), оба `friend`, `edt_shared_state.h`, `async_import_token`, `dockspace::set_on_*`. `editor_system` остаётся корнем композиции: создаёт сервисы и `editor_context` и регистрирует встроенное.

## 3. Пересечения с M3 (предлагаемый текст issues)

- **#137 (D):** «`document`/`document_manager`; `scene_document` из `shared_state` (`world_entry`, dirty, `commit`), `prefab_document` из `prefab_editor_context` (read-only, владеет `preview:*`); документ сцены владеет набором `level` (#12). Одна сцена + ≤1 превью; Save / Save As / Exit-prompt — как сейчас, через `dialog_service`.»
- **#31 (M3), остаток:** «несколько документов одновременно (вкладки, активный документ), Save All, Close с промптом на документ, data-документы, редактируемый `prefab_document` (вместе с #30)». Пункт «свой world-контекст» снять: он закрыт #12 + #137.
- **#138 (D):** «in-process реестры panel/action/inspector/asset_kind с RAII-регистрацией в `editor_context`; встроенное регистрируется через них; тестовое расширение в `unit_tests` добавляет панель, action и инспектор и снимает их».
- **#22 (M3), остаток:** «`registration_context` с областями для `game_editor.dll` (снятие при выгрузке и hot reload, #217); реестр превью ассетов; установка `snk/editor` в SDK и export-макросы; документация реестров; пример модуля во внешнем каталоге». В формулировке «реестры в `app_data_storage`» заменить на «через `editor_context` из `app_data_storage`».
- **#139 (D):** вместо критерия «на базе task_system» — «поверх `std::async`; прогресс, отмена, доставка на главный поток».
- **#135 (D, #17):** «бэкенд `job_service` → `task_system`, без правок в вызывающем коде».
- **#141 (M3):** «`command_manager` встраивается в `scene_document::commit`; `edt::command` вводится здесь». **#24:** панель регистрируется через `panel_registry` и берёт `diagnostics` из `job`.

## 4. Шаги (каждый — отдельный PR; поведение не меняется; CI: `unit_tests`, `editor-smoke` без правок)

Все PR — после #109 PR-5 (Editor не переезжает, но меняются его 173 include). Правки Editor идут строго последовательно с #12 PR-3 и PR-4: они трогают `shared_state`, контроллеры и viewport/prefab панели.
1. **PR-1 (#136).** `editor_context` и `editor_events` в `include/snk/editor/`. Хранятся в `editor_system` и `app_data_storage`; панели пока не меняются. Тест: сигналы, отписка в деструкторе.
2. **PR-2 (#136).** `selection_service`; провода hierarchy → inspector/viewport (`:119-125,238-261`) идут через события. Тесты: сброс выделения при `commit` и смене мира, поиск entity по пути.
3. **PR-3 (#139).** `dialog_service`; меню, confirm и save-as уходят из `init_helpers`; `m_confirm_action` удаляется. Тест с автоответами: New/Exit при dirty.
4. **PR-4 (#139).** `job_service` (`std::async`). Переводятся импорт, экспорт и опросы загрузки; `cancel_all_and_wait` при выходе. Тест на синхронном исполнителе: прогресс, отмена, `on_done` не зовётся после смерти owner.
5. **PR-5 (#138).** `panel_registry` и `action_registry`; меню и раскладка dockspace строятся из реестров (порядок и заголовки те же). Тесты переписываются с `panel_manager`.
6. **PR-6 (#138).** `inspector_registry` и `asset_kind_registry`; тестовое расширение (AC #138).
   — здесь ждём #12 PR-3…PR-5 —
7. **PR-7 (#137).** `document`, `document_manager`, `scene_document` (`shared_state` переименован, старое имя временно остаётся алиасом), `world_entry`, `commit` во всех 12 местах. Затрагиваются `scn_node_id_tests`, `edt_empty_level_tests`, `edt_world_array_sync_tests`.
8. **PR-8 (#137).** `prefab_document` с набором `preview:*`; камера переезжает в документ; второй `friend` удаляется.
9. **PR-9 (#140).** Удалить `init_helpers`, алиас `shared_state` и `set_on_*` у dockspace. Проверка: `grep -r "friend\|init_helpers\|shared_state" Editor/code` пуст.

## 5. Риски

- **Тихие изменения поведения.** Порядок вызовов в лямбдах (`on_after_push` делает `invalidate_view` до viewport, `:243`) нужно сохранить в порядке подписок. Защита — тесты PR-2 и smoke.
- **ImGui popup-id.** `OpenPopup`/`BeginPopupModal` должны вызываться в том же ID-стеке, поэтому `dialog_service` рисует внутри `register_implicit`, а не из панелей.
- **Время жизни.** Сейчас лямбды захватывают `&` на объекты `editor_system`. RAII-подписки и `owner` у job обязательны, иначе после закрытия превью пойдут висячие вызовы.
- **Конфликты.** #12 PR-3/PR-4 (`shared_state`, viewport) и #130 (инспектор). #137 нельзя вести параллельно с #112.
- **DLL позже (#22).** `editor_system` — STATIC. Шаблонные `run<R>` и `std::function` в API нужно держать на границе, иначе ABI для `game_editor.dll` придётся переделывать.
- **Покрытие.** `editor_system::init` тестирует только smoke. Новые сервисы должны собираться в тестах без окна и GL.

## 6. Вопросы владельцу

1. Публичные заголовки редактора — сразу в `include/snk/editor/` (рек.) или плоско, а переезд оставить #22?
2. Граница с Undo: в D только `commit(label)` и термин «action» для меню, а `edt::command` и стек — в #141 (рек.)? Или интерфейс `command` вводить уже сейчас?
3. Выделение по пути node id без сырых указателей (рек.) или в D оставить `prefab_node*` из hierarchy?
4. `job_service`: отмена кооперативная, импорт при отмене выбрасывает результат, прогресс остаётся одной модалкой как сейчас (рек.)? Или сразу делать немодальный список задач?
5. Реестры доступны через `editor_context` из `app_data_storage` (рек.) или каждый реестр кладётся в `app_data_storage` напрямую, как в тексте #22?
6. #137 ждёт #12 PR-5 (рек., `WORLD_CONTEXT` §4), а #136/#138/#139 идут раньше? Или делать #137 сразу на `level_manager` и потом переделывать?

> **Сверка с `TASK_SYSTEM.md` (до утверждения):** сигнатуры `job_service` в двух черновиках расходятся. Здесь: `run(label, work, on_done, weak_ptr owner)` + `poll()`. В TASK_SYSTEM: `start(task_desc, fn(task_context&))` по образцу `task_system::run`, с `then_main(ds::lifetime, …)`. Предлагается взять форму TASK_SYSTEM (`task_desc` + `task_context` + `ds::lifetime` вместо `weak_ptr<void>`), чтобы смена бэкенда в #135 не трогала вызывающий код.

## Решения владельца (2026-10-18, «по рекомендациям»)

1. Публичные заголовки редактора — сразу в `include/snk/editor/`.
2. В D только `scene_document::commit(label)` и термин «action»; `edt::command` и стек — в #141.
3. Выделение по пути node id, без сырых указателей.
4. `job_service`: отмена кооперативная, импорт при отмене выбрасывает результат, прогресс — одна модалка.
5. Реестры доступны через `editor_context` из `app_data_storage`.
6. #137 ждёт #12 PR-5; #136/#138/#139 идут раньше.
7. Сигнатура `job_service` — по `TASK_SYSTEM.md` (`start(task_desc, fn(task_context&))`, доставка через `ds::lifetime`), чтобы #135 менял только backend.
