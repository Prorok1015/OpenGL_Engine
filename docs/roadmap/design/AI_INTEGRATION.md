# AI_INTEGRATION — управление редактором и игрой из ИИ-агентов (MCP)

**Issue:** #284 · **Статус:** вопросы 2–6 утверждены владельцем 2026-10-19, вопрос 1 (транспорт) открыт · Основа: `EDITOR_FRAMEWORK.md` (#18), EPIC-15 undo (#29, #141–#146), `DISTRIBUTION.md` (Extension API #22, `registration_context`), `DESC_SCHEMA.md` (#128/#130), `WORLD_MESSAGING.md` (#268, `entity_ref`), детерминированные кадры #260, `play_session` EPIC-63 (#39), problems EPIC-51 (#24).

**Цель.** Любой MCP-клиент (Claude Code, Claude Desktop, Cursor и т.п.) полностью управляет редактором, а позже и запущенной игрой. Агент — ещё один «пульт» к тем же командам, что меню, хоткеи и тесты. Отдельного «ИИ-API» нет.

**Не скриптовый язык.** ADR в `CHIMERA_ROADMAP.md:42` («скриптового языка нет») не нарушается. Встроенного интерпретатора нет: это протокол удалённого вызова уже существующих команд.

## 1. Текущее состояние (aibranch, 2026-10-19)

| Область | Что есть | Пробел для агента |
|---|---|---|
| Меню и хоткеи | Меню — захардкоженный ImGui: `edt_dockspace.cpp:59-80` (File 61-67, View 71-75, Help 79). Ctrl+S опрашивается в `edt_dockspace.cpp:52`. Логика — в `edt::file_menu` (`edt_file_menu.cpp:52-121`), проводка через `dockspace::set_on_*` (`:123-131`). Контекстные действия разбросаны по панелям: hierarchy create/rename/duplicate/delete (`edt_scene_hierarchy_panel.cpp:361,566,573,589`, Delete :382), Add Component (`edt_inspector_panel.cpp:446`), Copy path (`edt_asset_browser_panel.cpp:205`) | Нет реестра. Open/Import/Save/Rename требуют ввода в ImGui |
| `action_registry` | Ветка PR-5 #138 (`wip/138-panel-action-registry`, `edt_action_registry.h:29-99`): `action_desc{id,label,menu_path,shortcut,execute,enabled}`, `run(id)`, `dispatch`. Действия `file.*` | `execute` без аргументов и результата, нет схем. Владелец добавил это в PR-5: §3.1 |
| Сервисы редактора | `editor_context` (`edt_editor_context.h:20-55`); `editor_events` (`edt_editor_events.h:137-147`: document_*, selection_changed, level_reloaded); `selection_service` (`edt_selection_service.h:24-33`); `dialog_service` (`edt_dialog_service.h:56-63`) с `dialog_auto_answer` (`:26`, только для тестов); `job_service` (`edt_job_service.h:44-75`) | Нет событий задач (start/progress/done). Автоответ — по категориям, а не по id диалога. Все сервисы только на главном потоке |
| Undo | Кода нет. EPIC-15 (`docs/epics/editor/EPIC-15.md`, #29): `command{execute,undo,description}`, `command_manager` (50 записей), cmd_add/delete_node/transform/set_field. `CHIMERA_ROADMAP.md:188`: стек на документ, save-point, слияние drag | Правки идут через 12 мест (`edt_scene_editor.cpp:41-216`, `edt_world_controller.cpp:139`, `edt_level_controller.cpp:84`, `edt_init_helpers.cpp:125,139`) и `serialize_and_push` (`edt_scene_editor.cpp:222`). Единой точки мутации нет; `scene_document::commit` появится в #137 |
| Состояние | desc-схемы реализованы: `desc_schema.h:162-202` (`schema_registry`, Boost.Describe), `desc_schema_ops.h:84-109` (serialize/diff/validate). Уровень в JSON — `level_controller::build_level_json` (`edt_level_controller.cpp:183`). `prefab_node::node_id` (`scn_prefab_desc.h:34`), `scn_node_index.h:13-26`. `ecs::entity_ref` (`ecs_channel.hpp:28-33`). Адрес в редакторе — `resolve_node_path` (`edt_scene_editor.h:53`) | Редактор не видит схемы (инспектор работает через `component_ui_registry`, #130 открыт). Нет get/set поля по пути, нет запросов с фильтром и пагинацией |
| Лог | spdlog: консоль + файл (`engine_log.cpp:79-103`). Один `set_log_callback` (`:196`), его занимает консоль (`edt_init_helpers.cpp:57`). Консоль хранит до 10000 `{level,message}` (`edt_console_panel.h:33`) | Нет номера записи, времени и категории, нет запроса «с N». Второго слушателя не повесить. Problems #24 не реализован |
| Кадры и скриншоты | `tools/run_headless.sh --frames N` → `app/fixed_frame_dt`, `app/freeze_after_frames` (`core_module.cpp:26-27`, `app_loop_service.cpp:29-44`). Снимок — ImageMagick `import -window root`, вне процесса. PNG-писатель есть (`image.cpp:94-155`). `render/backend=null` есть (`rnd_render_service_init.cpp:24-26`), null-окно — только в тестах | Нет захвата render target в процессе, нет «шагнуть N кадров» во время работы. Редактору нужен дисплей (Xvfb) |
| Расширения | Реестры строковые, `add` → RAII registration (`EDITOR_FRAMEWORK.md:116-124`). `registration_context` со скоупами — `DISTRIBUTION.md:45-57`, в коде пока нет | Tools должны регистрироваться тем же путём |
| Сеть и JSON | Boost 1.84: json, asio, program_options, test, describe, mp11 (`cmake/ThirdParty.cmake:72-90`). Asio — только io_context и thread_pool (`res_vfs_resolver.cpp`, `ds_task_system.cpp:89`). JSON — `boost::json` | Транспорта нет. TCP есть в asio без новых зависимостей; HTTP требует Beast |
| Play | `ecs::run_mode{edit,play,pause,step}` (`ecs_system_registry.hpp:20`), `context_manager::set_mode` (`scn_context_manager.h:100`). Редактор держит edit (`edt_editor_system.cpp:81`). `play_session` (#39) не начат. Игровой цикл без редактора — `app_engine_without_editor_tests.cpp:12-96` | Нет Play/Stop/Step ни для человека, ни для агента |

## 2. Решение в одном абзаце

В движке появляется **реестр команд** `cmd::command_registry`. У каждой команды есть id, версия, JSON Schema аргументов и результата, флаги опасности и `invoke(args) → result`. Редакторный `action_registry` — тот же реестр плюс UI-метаданные (меню, хоткей, подпись). Это одна сущность, а не две: §3.1.

Меню, хоткеи, тесты и агент вызывают `invoke`. Все вызовы извне маршалятся на главный поток и выполняются между кадрами.

Изменения сцены — только команды `command_manager` (#29). Вызовы агента идут с `origin = agent(session)` и собираются в транзакцию сессии.

Чтение состояния — тоже команды (`scene.query`, `node.get`, `log.read`, `viewport.capture`…). Поверх них — MCP resources.

Транспорт: в редакторе работает MCP-сервер на `127.0.0.1` с токеном. Для клиентов, которые запускают серверы через stdio, есть мост `snk_mcp`: он прокачивает байты между stdio и TCP.

Список tools строится из реестра автоматически. События редактора и задач становятся MCP notifications.

Общее (реестр, схемы, очередь на главный поток, снимок кадра, шаг N кадров, журнал лога) лежит в движке, им пользуются и тесты движка в процессе. Сам MCP-сервер — отдельный статический модуль уровня редактора без зависимостей от движка: в бинарник игры он попасть не может (решение 7, §3.7). Автоматизация запущенной игры (Play, N кадров, скриншот, сравнение) идёт через редактор.

## 3. API-набросок

### 3.1 Реестр команд (движок) и `action_registry` (редактор)

```cpp
namespace cmd {
enum class danger : uint8_t { none, confirm, deny_by_default }; // §5
struct command_desc {
	std::string id;                 // "file.save_as", "scene.node.create"; стабильный, точка — разделитель домена
	uint32_t version = 1;           // ломающее изменение схемы → version+1, старая версия живёт один релиз (§5)
	std::string summary;            // одна строка для агента и подсказки
	boost::json::value args_schema;   // JSON Schema (подмножество: type, properties, required, enum, min/max, items, $ref на desc-схемы)
	boost::json::value result_schema;
	danger risk = danger::none;
	bool mutates = false;           // true → идёт через command_manager (undo), §3.4
	bool long_running = false;      // true → результат {job_id}, завершение событием, §3.5
	std::function<result(const call&)> invoke;
	std::function<bool()> enabled;  // может ли команда выполниться сейчас
};
struct call { const boost::json::object& args; origin from; /* ui | test | agent{session} | game */ uint64_t expected_generation = 0; };
struct result { status code; boost::json::value value; std::string error; /* ok | invalid_args | disabled | stale | denied | needs_input | failed */ };
class command_registry {
	registration add(command_desc);                        // RAII, тот же паттерн, что registration_context (#22)
	result invoke(std::string_view id, const call&);       // проверяет схему, enabled, политику; только главный поток
	std::span<const command_desc* const> list() const;    // → MCP tools/list
};
class main_thread_queue { std::future<result> post(std::string id, boost::json::object args, origin); }; // любой поток → drain в стадии tasks
}
namespace edt {
struct action_ui { std::string label, menu_path, shortcut; bool separator_before = false; };
class action_registry { registration add(cmd::command_desc, std::optional<action_ui>); /* меню и хоткеи строятся из записей с ui */ };
}
```

Правила:
- **Нет действий только через ImGui.** Всё, что раньше спрашивалось в модалке (путь, имя, «сохранить?»), становится аргументом: `file.open{path}`, `file.save_as{path}`, `level.new{unsaved: save|discard|cancel}`, `node.rename{ref,name}`. Меню сначала собирает аргументы через `dialog_service`, потом вызывает `invoke`. Агент сразу передаёт аргументы.
- **Отсутствие обязательного решения.** Если для `level.new` без `unsaved` уровень грязный, результат — `needs_input` со схемой недостающего аргумента. Модалка у человека не открывается, агент не застревает.
- **Валидатор схем** — маленький модуль (`cmd_schema_validate`). Когда desc-схемы (#130) станут доступны редактору, `$ref: "desc:<type>"` раскрывается из `desc::schema_registry`. Компонент описывается один раз, и его JSON Schema генерируется из `type_schema`.
- **Где живёт.** `snk::command` — новый маленький модуль движка (зависит от common и boost::json). Игра без редактора регистрирует свои команды (`game.*`, `app.*`) в тот же тип реестра.

### 3.2 Чтение состояния (команды без мутаций)

| Команда | Аргументы → результат |
|---|---|
| `scene.query` | `{document?, world?, filter:{name_glob?, has_component?[], under?:ref}, fields?:[], page_size≤200, cursor?}` → `{items:[{ref,name,components[]}], next_cursor?}` |
| `node.get` | `{ref, components?:[]}` → `{ref, name, parent, children[], components:{type: desc-json}}`. Формат компонента — desc-схема (#128) |
| `component.schema` | `{type}` → JSON Schema из `type_schema` |
| `selection.get` / `document.list` / `document.get` | текущее выделение (`selection_service`), открытые документы и generation (#137) |
| `asset.query` | `{glob, kind?, page_size, cursor}` → теги `res://`, kind, размер |
| `log.read` | `{since_seq?, min_level?, category?, limit≤500}` → `{entries:[{seq,time,level,category,text}], next_seq}` |
| `problems.list` | диагностики #24 (ресурс + JSON-путь + причина) |
| `viewport.capture` | `{viewport?:"scene"\|"game"\|<id>, width?, height?}` → PNG (MCP image content). Захват render target в процессе: §3.6 |
| `app.advance` | `{frames, fixed_dt?}` → `{frame}`. Детерминированный шаг, как в #260; в редакторе — только в Play/Pause |

**Адресация `ref`:** `{"context": "<set>/<world>", "path": [node_id, …]}` — это JSON-форма `ecs::entity_ref` (`ecs_channel.hpp:28-33`), а `path` — цепочка `node_id` из `scn_node_index` и `resolve_node_path`. Внутри префаба путь продолжается через override-записи. Имена служат только для показа и не годятся как ключ. В ответах `ref` всегда полный, чтобы агент мог вернуть его как есть.

Пагинация обязательна. Курсор непрозрачный, привязан к generation документа; после правки старый курсор даёт `stale`.

### 3.3 Изменения

`scene.node.create/delete/duplicate/reparent/rename`, `component.add/remove`, `component.set{ref, type, patch}`. `patch` — JSON Merge Patch, проверяется `desc::validate_override`. Сюда же `transform.set`, `file.*`, `asset.import{path}` (long_running), `asset.export`.

Все такие команды помечены `mutates=true` и идут через `command_manager`: каждый вызов — одна команда undo с подписью из `summary` и аргументов.

**Оптимистическая проверка.** Аргумент `expected_generation` (generation документа из `document_changed`) необязателен. Если он задан и не совпал, результат — `stale` с текущим generation, и агент перечитывает состояние.

### 3.4 Undo и сессия агента

- Нужен `command_manager` (#29, #141–#146): стек на документ, save-point, слияние drag.
- `agent.session.begin{label}` открывает **транзакцию**: все команды агента в документе складываются в одну составную запись undo. `agent.session.end` её закрывает, `agent.session.rollback` откатывает целиком. `agent.session.checkpoint` дробит сессию на шаги.
- Если человек правит во время сессии, его команды в транзакцию не попадают. Откат сессии отменяет только команды агента и только пока они не перекрыты правками человека тех же узлов. Иначе откат останавливается на конфликте и сообщает, что не удалось (§5, Q5).
- Запись в истории и узлы, изменённые агентом, помечаются «изменено агентом» (иконка в history, hierarchy и inspector) до сохранения или до явного «принять».

### 3.5 События и длинные операции

- `editor_events` → MCP `notifications/resources/updated` по подписке: `snk://selection`, `snk://document/<id>` (generation), `snk://log` (новый seq), `snk://problems`.
- `job_service` получает события `job_started/progress/finished{id, status, result|error}` (сейчас их нет). Команда с `long_running=true` сразу возвращает `{job_id}`. Прогресс идёт через MCP `notifications/progress` по `progressToken` вызова, завершение — notification плюс `job.get{id}` и `job.cancel{id}`.
- `dialog_service`: у каждого запроса есть id и схема ответа. Команды `dialog.list` / `dialog.answer{id, answer}` дают программный ответ на модалку, которую открыл человек или старый код. `dialog_auto_answer` (`edt_dialog_service.h:26`) остаётся для тестов.

### 3.6 Общее в движке: кадр, снимок, журнал

- **Захват кадра.** `rnd::driver::read_texture(texture_handle, rect) → rgba8 bytes`: в GL — `glGetTextureSubImage`, в null — заполнитель, в validation — проверка состояния текстуры. `rnd::capture_service` захватывает финальный render target вида (viewport редактора или камеры игры) на границе кадра и кодирует PNG существующим писателем (`image.cpp`). Внешний `import -window root` остаётся в CI как сверка.
- **Шаг кадров.** `app::frame_control{advance(n), fixed_dt}` — обобщение `app/freeze_after_frames` из #260 до команды во время работы.
- **Журнал лога.** Кольцевой буфер в `engine_log`: seq, время, уровень, категория. `set_log_callback` становится списком sink-ов, чтобы консоль и агент не вытесняли друг друга.
- **Очередь на главный поток.** `cmd::main_thread_queue` поверх `ds::task_system` main drain; второго drain нет.
- **Без дисплея.** Редактор на `render/backend=null` с null-окном даёт команды и чтение без GPU (CI, QA). Для `viewport.capture` нужен реальный GL (Xvfb/llvmpipe).

### 3.7 MCP-сервер — отдельный модуль уровня редактора (решение владельца 7)

**Модуль `snk_mcp_server`** — отдельная CMake-цель `STATIC` в `Editor/code/mcp_server/`:
- Объявляется только в CMake редактора. Линкуется только в `editor` (и в свои тесты, а также в мост `snk_mcp`, если он понадобится).
- **Ни от чего из движка не зависит:** ни `snk::common`, ни `egLOG`, ни `ds::task_system`. Только сторонние Boost.JSON и Boost.Asio (и Beast, если транспорт — HTTP; см. Q1).
- Цель: при сборке игры сервер технически не может попасть в бинарник.

**Граница — интерфейс, который реализует редактор:**

```cpp
namespace mcp {
struct tool_info { std::string name, description; boost::json::value input_schema, output_schema; bool destructive = false; };
struct call_result { bool is_error = false; boost::json::value structured; std::vector<content> content; }; // text | image(png base64)
class host { // реализует редактор; модуль вызывает его со своего io-потока
public:
	virtual ~host() = default;
	virtual std::vector<tool_info> list_tools() = 0;
	virtual void call_tool(std::string name, boost::json::value args, progress_sink, std::function<void(call_result)> done) = 0; // async
	virtual std::vector<resource_info> list_resources() = 0;
	virtual void read_resource(std::string uri, std::function<void(resource_result)> done) = 0;
	virtual void log(level, std::string_view) = 0; // модуль не знает egLOG
};
class server { public: server(host&, server_config); void start(); void stop(); void notify_resource_updated(std::string uri); void notify_tools_changed(); };
}
```

- **Адаптер на стороне редактора** (`edt::mcp_bridge_host`) переводит `host` в `cmd::command_registry`/`action_registry`. Вызовы маршалятся на главный поток через `main_thread_queue`. Сюда же входят политика, подтверждения и пометка `origin=agent`. Модуль про это не знает.
- **Внутри модуля:** протокол MCP (initialize, tools/list|call, resources/list|read|subscribe, notifications/*, progress), транспорт, токен, проверка `Origin`/`Host`, лимиты размера сообщений. Модуль тестируется отдельно, на фейковом `host`, без движка.
- **CI-проверка** (по образцу `check_no_gl_outside_drivers.sh`): цели движка, игры и SDK не линкуют `snk_mcp_server`; SDK-пакет его не устанавливает.
- **Транспорт** — вопрос Q1, открыт. Рекомендация: Streamable HTTP в модуле + необязательный stdio-мост.
- `tools/list` = `command_registry::list()` через адаптер, кроме выключенных политикой и `expose_as_tool=false`. Имя tool — id с заменой `.` → `_`, плюс `_v<N>` при version>1. `inputSchema`/`outputSchema` = схемы, `annotations.destructiveHint` = `danger`/`mutates`.
- `resources`: `snk://scene/<doc>` (сводка), `snk://node/<ref>`, `snk://assets`, `snk://log`, `snk://problems`, `snk://viewport/<id>.png`, `snk://schema/<type>`.
- `notifications`: §3.5, плюс `notifications/tools/list_changed`, когда расширение добавило или убрало команды.
- **Игра без редактора.** Сервера в игровом бинарнике нет. Автоматизация игры (Play, N кадров, скриншот, сравнение) идёт через редактор: `play_session` (#39) поднимает набор `play`, а команды `play.*`, `app.advance`, `viewport.capture{viewport:"game"}` выполняются в процессе редактора. Реестр команд, сериализация, захват кадра и шаг кадров остаются в движке (принцип 7), так что ими пользуются и тесты движка в процессе (`app_engine_without_editor_tests`). Вопрос Q7 — нужен ли отдельный dev-хост для автоматизации без редактора.

## 4. Стыковка с другими эпиками

| С чем | Как |
|---|---|
| EDITOR_FRAMEWORK #18 | `action_registry` (PR-5 #138) с первого дня несёт схемы и `invoke(args)`; меню собирает аргументы через `dialog_service`. `job_service` (#139) получает события. Documents (#137) дают `document_id`, generation и `commit` — основу `expected_generation` и undo |
| Undo M3 #29 (#141–#146) | Все `mutates`-команды — команды `command_manager`. Сессия агента — составная транзакция. Пометка «изменено агентом» рядом с save-point (#144) |
| Extension API #22 | Адаптер `mcp_bridge_host` и команды расширений регистрируются через `registration_context`. Выгрузка `game_editor.dll` снимает её команды и шлёт `tools/list_changed` |
| problems #24 | `problems.list` и `snk://problems` читают тот же сборщик диагностик |
| desc-схемы #128/#130 | JSON Schema компонентов генерируется из `type_schema`. `component.set` проверяется `validate_override`. Формат `node.get` — desc-JSON |
| WORLD_MESSAGING #268 | `ref` = JSON-форма `ecs::entity_ref{context, path}`. Команды игры могут писать во внешний blackboard канала (`set_channel::external_blackboard`) |
| Детерминизм #260 | `app.advance{frames}` и `viewport.capture` дают те же кадры, что `run_headless.sh --frames`. Сравнение со эталоном — команда `capture.compare{reference, fuzz}` по логике `compare_shots` |
| play_session #39 | `play.start/stop/pause/step{n}` — команды над `play_session`. До EPIC-63 их нет |

## 5. Безопасность и совместная работа

- **Сеть.** Только `127.0.0.1`, токен на запуск, файл с правами пользователя. Без токена соединение рвётся до первого сообщения. Сервер выключен по умолчанию (`editor/agent_server=false`), его включает пользователь.
- **Опасные действия.**
  - `danger::confirm`: удаление файлов, перезапись вне проекта, сборка (`game.dll`), запуск внешних процессов, выход без сохранения. Человек видит немодальный запрос «агент хочет …» с таймаутом 60 с, отказ или таймаут дают `denied`.
  - `danger::deny_by_default`: произвольные пути вне проекта, запуск кода.
  - Белый список проекта `<project>/.snk/agent_policy.json` (`allow: ["asset.import", "build.game"]`) снимает подтверждение. Его правит только человек; команды, меняющей политику, нет.
- **Одновременная правка.**
  - Все вызовы выполняются на главном потоке между кадрами, гонок нет.
  - Пока у человека идёт жест (drag гизмо, ввод в поле), мутирующие команды агента ждут в очереди до конца жеста (таймаут → `busy`).
  - Конфликты ловит `expected_generation`.
  - В статус-баре видно, что агент подключён и что он сейчас делает; есть кнопка «отключить агента».
- **Версии схем.** `version` у команды. Ломающее изменение заводит `<id>` с новой версией, старая живёт один релиз с пометкой `deprecated` в описании. Сервер сообщает `serverInfo.snk_api` — версию набора команд.
- **Объём.** Запросы с фильтром и пагинацией, `page_size` ограничен, выгрузки сцены целиком нет. Скриншот ограничен размером (по умолчанию 1280 px по длинной стороне).

## 6. PR и порядок

Текущая волна (PR-6 #12, R5a/R5b, EDITOR PR-5…PR-9) не останавливается.

| PR | Что | Когда |
|---|---|---|
| **A0** (#138) | Схемы и `invoke(args)` у `action_registry`, нет действий только через ImGui, тесты headless-вызова. В PR-5 или сразу после | сейчас, в волне |
| A1 | Модуль `snk::command`: реестр, валидатор схем, `main_thread_queue`. `action_registry` поверх него | после PR-5 |
| A2 | Журнал лога (seq, sink-список), события `job_service`, id и `dialog.answer` у `dialog_service` | после A1, параллельно с R5b |
| A3 | Захват кадра: `read_texture`, `capture_service`, `app::frame_control` (`app.advance`) | после R5b |
| A4 | Команды чтения: `scene.query`, `node.get`, `component.schema`, `asset.query`, `selection.get`. Требует documents #137 (PR-7/8) и доступа к схемам #130 | после #137 |
| A5 | Мутирующие команды через `command_manager`, `agent.session.*`, пометка «изменено агентом» | после undo #29 (#141–#146) |
| A6a | Модуль `snk_mcp_server` (протокол, транспорт, токен, тесты на фейковом `host`) + CI-проверка «не линкуется в движок/игру/SDK»; адаптер редактора, только чтение и скриншоты (решение 2) | после A3 |
| A6b | Мутирующие tools, политика, подтверждения, пометка «изменено агентом» в адаптере | после A5 |
| A7 | Автоматизация игры через редактор: `play.*`, `app.advance`, `viewport.capture{game}`, `capture.compare` | после EPIC-63 #39 |

Сервер (A6) стоит после команд, undo и documents, как просил владелец. Q2 предлагает одно обоснованное исключение.

## 7. Риски

- **Широта.** Без единой точки мутации (сейчас 12 мест) undo и транзакция сессии невозможны, всё упирается в #137/#141. Ранний MCP с мутациями был бы хаком.
- **Схемы дважды.** Ручные JSON Schema у команд и desc-схемы могут разойтись. Компоненты описываются только через `$ref: desc:<type>`.
- **Главный поток.** Медленная команда агента (большой `scene.query`) роняет FPS. Нужны лимиты страницы и бюджет времени на drain (как `budget_*` в validation).
- **Тяжёлый скриншот.** `glGetTextureSubImage` синхронизирует GPU. Захват только по запросу и на границе кадра; PBO-асинхронность — позже.
- **Безопасность.** Локальный порт доступен другим процессам пользователя; токен и права файла это закрывают, но не от вредоносного кода под тем же пользователем. Это граница модели угроз, её стоит явно записать.
- **Совместимость MCP.** Спецификация меняется (транспорты, outputSchema). Протокольный слой изолирован в `mcp_server`, реестр от него не зависит.
- **Пустой кадр.** Детерминизм снимков держится на #260; в редакторе вне Play кадр зависит от UI. Сравнение эталонов — только в Play или в игре.

## 8. Вопросы владельцу

1. **Транспорт:** TCP localhost + токен в редакторе и stdio-мост `snk_mcp` (без новых зависимостей) или сразу Streamable HTTP через Boost.Beast? **Рекомендация:** TCP + мост; HTTP добавить, когда понадобится удалённый клиент.
2. **Ранний read-only сервер:** выпустить A6 в режиме «только чтение и скриншоты» сразу после A3, до undo и documents? Мутирующие tools в этом режиме выключены. **Рекомендация:** да. Чтение не требует undo, а скриншоты и лог сразу полезны для QA и отладки. Мутации включаются в A5.
3. **Где реестр:** новый модуль движка `snk::command`, а редакторный `action_registry` = он + UI-метаданные? **Рекомендация:** да (принцип 7). Иначе игре без редактора нужен второй реестр.
4. **Человек и агент одновременно:** очередь команд агента на время жеста человека + `expected_generation`, или блокировка документа на время сессии агента? **Рекомендация:** очередь + generation. Блокировка мешает человеку и не нужна при выполнении на главном потоке.
5. **Откат сессии при пересечении с правками человека:** откатывать только команды агента до первого конфликта и сообщать о нём, или откатывать всё после начала сессии, включая правки человека? **Рекомендация:** только команды агента, остановка на конфликте.
6. **Опасные действия:** набор `confirm` / `deny_by_default`, как в §5, с белым списком проекта в `.snk/agent_policy.json`, который правит только человек? **Рекомендация:** да. Плюс сервер выключен по умолчанию.

## Решения владельца (2026-10-19)

1. Транспорт — **открыт**, разбирается отдельно (см. обсуждение плюсов и минусов).
2. Ранний сервер только для чтения (сцена, лог, скриншоты) — да, сразу после A3; мутирующие tools включаются в A5.
3. Реестр команд — модуль движка `snk::command`; `action_registry` редактора = он + UI-метаданные.
4. Одновременная правка — очередь команд агента на время жеста человека + `expected_generation`; без блокировки документа.
5. Откат сессии — только команды агента, остановка на первом конфликте с правкой человека и отчёт.
6. Опасные действия — `confirm` / `deny_by_default` как в §5, белый список `.snk/agent_policy.json` правит только человек, сервер выключен по умолчанию.
7. MCP-сервер — отдельный CMake-модуль `STATIC` на уровне редактора, без зависимостей от движка, линкуется только в редактор; в бинарник игры технически попасть не может (§3.7).

**Открыто:**
- Q1 — транспорт (рекомендация: Streamable HTTP в модуле + необязательный stdio-мост).
- Q7 — автоматизация игры без редактора: только через редактор (рекомендация) или отдельный dev-хост в `tools/`, который линкует модуль и не является игрой.
