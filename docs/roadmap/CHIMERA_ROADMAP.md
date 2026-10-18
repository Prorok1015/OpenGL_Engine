# Роадмап развития движка под ТЗ «Химера» v0.1

**Статус:** черновик для согласования
**Основание:** `Chimera_Engine_Requirements_0_1` (17.10.2026), аудит кода ветки `aibranch` (ревизия 2 — коммит `80d4e86`, после EPIC-49 и EPIC-41 US-41-1/4/6)
**Следующий шаг:** детальная проработка каждого технического решения (ADR) → эпики в `docs/epics/`

> **Принятые архитектурные решения** (платформа Windows + OpenGL с бэкенд-независимым RHI, GLFW за `platform::window`, только собственные графические бэкенды, GLSL с правилами переносимости, явные фазы ECS, префикс `m_`) и план расчистки до работ по ТЗ — в [ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md) §7, §9. Где ADR этого документа расходятся с ними (ADR-03, ADR-08, упоминания SDL3/Slang), действует ARCHITECTURE_REVIEW.

> **Ревизия 2 (после EPIC-49).** Номера предложенных эпиков сдвинуты на +1 (EPIC-49 занят: «Engine Componentization & Module Wiring», done). EPIC-49 закрыл часть M0 (слои, `depends_on`-топосорт модулей, общий `frame_stage_registry`, `*_api` таргеты, развязка scene↔render, выдача id в превью префаба) — это учтено в §2, §3, §5 (M0). Не изменились: двойной запуск систем в `run_graph_recursive`, пустой fixed-граф, падение на неизвестном `__type`, абсолютные пути в CMake. В M0 добавлены пункты бэклога BL-002…BL-006 и CI (BL-003). Функциональная часть роадмапа (M1–M6, ADR-02, 04–07, 09–13) без изменений.

---

## 1. Рамки

Заказчик делает игру во **внешнем проекте**, используя установленный редактор и SDK. Движок отвечает за проекты, сцены, префабы, desc, UI, ресурсы, Play, сохранение, сборку, API расширений, фоновые задачи и диагностику. Правила игры (карты, паки, скрещивание, экономика) — зона игры, их нельзя молча превращать в ограничения движка.

Игра — коллекционная карточная, по сути 2D/2.5D. Значит: **3D-рендер не на критическом пути**, а UI, данные, ресурсы-изображения, Play и сборка — на критическом.

Этап А — минимум для создания и проверки игры (AC01–AC06 в объёме А). Этап Б — GUI06, AUD01, LOC01 и требования выпуска.

---

## 2. Принципы развития (что сохраняем)

План — эволюция текущей архитектуры. Опорные решения, которые уже есть и на которые всё ложится:

| Существующий механизм | Во что развивается |
|---|---|
| **desc как источник правды**, ECS собирается из desc (`ecs_assembler`, `reload_world`) | Play = новая сборка ECS из копии desc в изолированном `scn::level`; data-документы (карта, пак, правила) = desc без сцены |
| `app::modules::module_interface` с `name()` + `depends_on()` и топосортом в `module_loader` (EPIC-49 US-49-7) | Контракт игрового модуля: `game` объявляет `depends_on{core, engine}`, `game_editor` — `{editor, game}`; порядок init/shutdown получается сам |
| `gs::frame_stage_registry` — общий для editor и `gs_loop_service` (EPIC-49 US-49-8) | Стадии Play-gate, UI, дренаж main-thread очереди задач — по одной `add_stage`, без правки циклов; player переиспользует тот же реестр |
| `snk::core_api` / `snk::engine_api` INTERFACE-контракты, `snk::` алиасы, `CMakePresets.json` (EPIC-49, EPIC-41) | Основа SDK: экспортируемые таргеты `SnakeEngine::engine_api`, install только публичных заголовков |
| Инъекция менеджеров через `frame_context`, POD `material_desc` + `rnd::material_compiler_interface` (US-49-5/6) | Шаблон отказа от синглтонов для остальных `get_system()`; UI-пасс получает менеджеры так же |
| `ds::app_data_storage` | Публикация extension API: реестр панелей, ассетов, команд, renderer'ов инспектора |
| `desc::desc_system::register_desc<T>` + `ecs_assembler::register_desc_spawner` + `system_factory` | Реестр типов со схемой полей (метаданные, версии, валидация); старые desc работают через мост |
| VFS `vfs_resolver` со стеком mount points, протоколы `res://`, `memory://` | Слоёный `res://` (проект поверх движка), протоколы `engine://`, `edt://`, `cache://`, `user://` |
| `cfg` пути относительно `.cfg` файла | Манифест проекта как cfg-совместимый файл с относительными путями |
| `res_control_block` (статус/ошибка/`then`) + `adapter_worker` (asio) | Общая система задач: пул, main-thread очередь, прогресс, отмена, токены времени жизни |
| Data-driven render pipeline (экстракторы → пассы), per-camera RT | UI-пасс и 2D-пасс как новые пассы; превью ассетов как отдельные камеры/RT |
| `component_ui_registry`, `panel_manager` | Публичные реестры редактора, inspector на схеме |

Чего **не** делаем: не меняем ECS (EnTT остаётся), не переписываем рендер, не уходим от JSON-desc, не вводим скриптовый язык (визуальное программирование вне объёма).

---

## 3. Текущее покрытие требований

Легенда: ✅ есть · 🟡 частично · ❌ нет

### 3.1 Проект и поставка

| ID | Ст. | Что есть | Главный разрыв |
|---|---|---|---|
| PRJ01 | ❌ | Корень ресурсов задаётся `--config`/`--set resource/path` | Нет понятия проекта, create/open/close, recent |
| PRJ02 | ❌ | — | Нет манифеста (стартовая сцена, корни, расширения, платформа, версия движка) |
| PRJ03 | ❌ | — | `memory://` → `res/../sandbox`, данные редактора лежат в `Editor/res`, нет user/cache/build/save-каталогов |
| PRJ04 | 🟡 | Контент можно править внешним `res`, если рядом движковые шейдеры и base-desc; есть `core_api`/`engine_api` контракты | Нет слоя движковых ресурсов, нет SDK: install/export нет, физический `public/` split заголовков отложен (US-49-1 этап 2), `BUILD_SHARED_LIBS` принудительно OFF |
| PRJ05 | ❌ | — | Нет версий движка и форматов |
| PRJ06 | ❌ | Source build — единственный путь | Нет установленного варианта для сравнения |
| BLD01 | ❌ | `gs_loop_service` переведён на общий `frame_stage_registry`; движок конфигурируется из корневого CMake, а не из `editor_system` (US-49-2) | Нет player-исполняемого файла; абсолютные пути всё ещё вшиты (`SE_RESOURCE_ABSOLUTE_PATH`, `PROJECT_SOURCE_PATH` — EPIC-41 US-41-8 открыт); в `gs_loop_service` нет `frame_allocator_reset` (BL-005) |
| BLD02 | ❌ | — | Нет валидации, cook/package, исключения editor-only |

### 3.2 Данные и расширения

| ID | Ст. | Что есть | Главный разрыв |
|---|---|---|---|
| DSC01 | 🟡 | Публичные `register_desc`, `register_desc_spawner`, `system_factory`; модуль может объявить зависимости (`depends_on`) | Модули всё ещё перечислены в `Editor/main.cpp:53-55`; реестр UI приватен |
| DSC02 | 🟡 | `__parent` даёт шаблон/умолчания | Редактор открывает только `*.prefab.desc` |
| DSC03 | ❌ | JSON хранит всё; inspector-fallback по типу JSON-значения | Нет схемы/метаданных (`desc_hints.hpp` не используется), строки read-only, нет add/remove в списках, enum, optional |
| DSC04 | 🟡 | `__parent`, overrides префабов | Ссылки — нетипизированные строки-пути; нет пикера, нет «итог vs локальное», нет reset |
| DSC05 | ❌ | Ошибка JSON-синтаксиса ловится и логируется | Неизвестный `__type` → разыменование nullptr (`desc_adapter_t.cpp:20,28`); нет `__type` → ASSERT, который в release компилируется в пустоту (BL-002); нет детекции циклов; нет пути к полю |
| DSC06 | ❌ | — | Нет editor extension API |
| DAT01 | ❌ | — | Нет версий форматов и миграций |

### 3.3 Редактор

| ID | Ст. | Что есть | Главный разрыв |
|---|---|---|---|
| EDT01 | ❌ | Один уровень, один `is_dirty`, один prefab-превью | Нет модели документов |
| EDT02 | ❌ | Gizmo коммитит в desc один раз за drag (удобно для команды) | Нет command stack (EPIC-15 planned) |
| EDT03 | ❌ | file_watcher есть | Уровень редактируется как `memory://` копия → внешние изменения не видны; нет autosave |
| AST01 | 🟡 | Браузер, поиск по имени, drag в viewport | Нет видов ассетов, фильтров, миниатюр, create-действий, пикера |
| AST02 | ❌ | — | Идентичность ассета = путь, узла = имя; нет графа зависимостей |
| AST03 | 🟡 | Асинхронный импорт моделей + экспорт | Нет импорта изображений, настроек, `.meta`, повторного импорта |
| SCN01 | 🟡 | Иерархия, inspector, gizmo, duplicate, reparent, multi-select | Нет picking кликом; имя как идентичность |
| SCN02 | 🟡 | Изолированное превью префаба (EPIC-25 шаг 1) | Нет редактирования/сохранения/создания; Apply/Revert — заглушки |
| SCN03 | 🟡 | `prefab_instance.source_prefab`, hot reload сохраняет локальные overrides | Нет пополевых overrides, Apply/Revert; при drop из браузера связь теряется |
| SCN04 | 🟡 | Отдельный world превью, per-camera RT; превью берёт id через `acquire_free_world_id()` | Общий level/organizer/dt/input; `reload_world` стирает системы чужих миров; `edt_world_controller.cpp:78` всё ещё выдаёт id по `get_world_count()`; лимит 5 миров |
| SCN05 | ❌ | — | Каждая правка пересобирает мир; выделение — сырой `prefab_node*` + поиск по имени |
| SCN06 | ❌ | Материалы правятся через generic JSON | Нет редактора материалов, ошибки шейдеров только в лог |

### 3.4 Runtime

| ID | Ст. | Что есть | Главный разрыв |
|---|---|---|---|
| RUN01 | ❌ | — | Системы тикают всегда, Play/Stop/Pause/Step нет |
| RUN02 | ❌ | — | Редактируемый мир = симулируемый мир |
| RUN03 | 🟡 | Порядок систем задаётся desc + organizer; стадии кадра editor и player общие (US-49-8) | **Баг (не исправлен в EPIC-49):** `run_graph_recursive` без visited-set — система с ≥2 предшественниками исполняется несколько раз за кадр; `m_fixed_organizer` никогда не заполняется (fixed step пуст); нет RNG/seed; dt без clamp |
| TSK01 | 🟡 | Асинхронные загрузки, `std::async` в редакторе | Нет общей системы задач, прогресса, отмены; `then()` вызывается на рабочем потоке без маршалинга и без защиты времени жизни; гонка публикации `data` и TOCTOU в `then()` (BL-006) |
| IMG01 | 🟡 | PNG из памяти, `generate_texture` | Нет замены на месте (сырые `texture_interface*`), миниатюр, заглушек, бюджета/LRU |
| SAV01 | 🟡 | `store()` пишет `.tmp` + rename | Нет подтверждения, ошибка = ASSERT, нет fsync/бэкапа, коллизия имён `.tmp` |
| DIA01 | 🟡 | spdlog по категориям, профайлер (выключен по умолчанию) | Нет структурного контекста операции, problems-панели, метрик памяти/задач; ассерты исчезают в release (BL-002); нет периметра исключений в `main` и отката init (BL-004) |

### 3.5 Игровой UI и этап Б

| ID | Ст. | Разрыв |
|---|---|---|
| GUI01–05 | ❌ | Есть только ImGui для редактора. Нет UI-ресурса, layout, привязки данных, действий, игровых шрифтов (кириллица отсутствует — `AddFontDefault`), DPI только при старте |
| GUI06 (Б) | ❌ | Нет tween-анимаций |
| AUD01 (Б) | ❌ | Нет аудио-библиотеки |
| LOC01 (Б) | ❌ | Нет строковых таблиц |

**Итог:** ни одно требование не закрыто полностью. EPIC-49 сильно подготовил почву (слои, модульные зависимости, общий frame loop, контракты таргетов), но функциональных разрывов ТЗ не закрыл — он структурный. Сильная база — desc-модель, модульная система, VFS, data-driven рендер. Самые большие разрывы — проект/SDK/сборка, схема данных и идентичность, модель документов, Play, игровой UI.

---

## 4. Ключевые технические решения (вынести в ADR на следующем шаге)

Раздел 9 ТЗ требует определить эти решения в техническом проекте. Здесь — рекомендуемое направление; детали, альтернативы и обоснование — в отдельных ADR.

| ADR | Вопрос | Рекомендуемое направление | Почему это эволюция |
|---|---|---|---|
| ADR-01 | Модель подключения игрового кода | Движок (`core`+`engine`) как **shared library** в SDK; игра — `game.dll` (runtime) и `game_editor.dll` (editor-расширение), загружаемые установленным редактором при открытии проекта; player загружает только `game.dll`. Точка входа — фабрика `module_interface`. Без hot reload (вне объёма) | Контракт уже есть (`module_interface` + `depends_on`, заготовка `app_game_module_service_interface`). `rnd::get_system()` из scene/пассов уже убран (US-49-6); остаются `res::get_system()` (в т.ч. 11 вызовов в `scn_model_importer_adapter.cpp`), `gui::get_system()`, `gs_game_system`, `cfg::instance`, skinning на assembly-time (BL-007) — их убирает EPIC-29. Для shared library нужны export-макросы и снятие `BUILD_SHARED_LIBS OFF FORCE` для движковых таргетов |
| ADR-02 | Формат проекта и каталоги | `project.snkproj` (JSON): `engine_version`, `format_version`, `start_scene`, `content_roots`, `modules`, `platforms`. Протоколы: `res://` (слоёный: проект → движок), `engine://`, `edt://`, `cache://` (`<project>/.cache`), `user://` (ОС-каталог данных игрока, в Play — тестовый профиль), сборка в `<project>/build` | `vfs_resolver` уже держит стек mount points; EPIC-36/42 задуманы так же, но EPIC-42 нужно переработать: проект **вне** репо движка, с версией и разделением каталогов |
| ADR-03 | Распространение редактора и SDK | `cmake --install` → версионированный каталог `Snake/<ver>/` (bin, include, lib, cmake config `find_package(SnakeEngine <ver>)`), закреплённый toolchain: Windows x64 + MSVC (VS 18) — фактическая среда разработки по handoff EPIC-49. Source-путь: тот же проект, `SNAKE_ENGINE_DIR` указывает на локальную сборку | `snk::` алиасы, `*_api` INTERFACE-таргеты и пресеты уже есть; нужно добавить install/export и закончить физический `public/` split (иначе в SDK уедут внутренние заголовки) |
| ADR-04 | Схема desc и метаданные | Реестр типов в `desc_system`: для каждого типа — фабрика, **схема полей** (имя, вид, умолчание, диапазон, подсказка, enum, тип ссылки, optional), флаги `is_component`/`is_document`, версия. Описание полей через Boost.Describe (header-only, уже в Boost 1.84) + таблица атрибутов; generic serialize/deserialize/copy и inspector из схемы. Рукописные desc продолжают работать (мост) и мигрируют постепенно | Расширяет существующий `register_desc<T>`; оживляет неиспользуемый `desc_hints.hpp` |
| ADR-05 | Идентичность и ссылки | GUID ассета: `__guid` в `.desc`, sidecar `.meta` для png/glb; **asset database** (guid ↔ путь, обратные зависимости) в `cache://`. Ссылка сериализуется как `{guid, path}` — путь для читаемости, guid для устойчивости. Узлы префабов — стабильный `__id`, overrides и выделение по нему. В ECS — `stable_id_component` + индекс в ctx мира | `res::tag` остаётся рантайм-ключом; GUID разрешается в tag на этапе загрузки |
| ADR-06 | Версии и миграции | `__version` на каждый desc-тип + цепочка JSON→JSON мигратров в `desc_adapter_t` до `create_instance`; игра регистрирует мигратры своих типов. Миграция проекта — только явная команда, с бэкапом и отчётом | Единая точка входа — `desc_adapter_t::deserialize` |
| ADR-07 | Изоляция Play | `play_session` владеет **отдельным `scn::level`**, собранным из копии desc текущих документов; своё время (pause/step/time-scale), свой input, `user://` → тестовый профиль, отписан от hot reload. Stop = уничтожение level. Запись в авторский контент невозможна по построению | Desc-модель уже это даёт; отдельный level обходит лимит 5 миров (id миров — на level) |
| ADR-08 | Планировщик систем | Фазы (`input → fixed_sim → update → late → presentation`), фазы в `world_desc.systems`, visited/топологический порядок в `run_graph`, заполнение fixed organizer, clamp dt и лимит шагов, seeded RNG как ресурс level. Pause/Step — условие на стадии `update` в `frame_stage_registry` | Чинит текущий `scn::level::update`, не меняя organizer; уровень кадра уже упорядочен EPIC-49 |
| ADR-09 | Игровой UI | **Retained-дерево элементов как desc** (`ui_document`, элементы-компоненты, шаблоны = префабы UI через `__parent`/инстансы), layout на flexbox (**Yoga**, MIT) + якоря, рендер — новый UI-пасс в data-driven pipeline (батчинг квадов + SDF/растровые шрифты через **FreeType** или stb_truetype), привязка данных через пути к полям схемы ADR-04, действия — реестр `ui_action` из игрового модуля. Альтернатива для оценки: RmlUi (HTML/CSS, есть биндинги и анимации, но формат вне desc и нет визуального редактора) | Переиспользует desc, inspector, undo, prefab-overrides, render passes — визуальный редактор UI получается как ещё один тип документа |
| ADR-10 | Фоновые задачи | `ds::task_system`: asio `thread_pool` + main-thread очередь, дренируемая в loop service; `task_handle` (статус, прогресс, ошибка, отмена, `std::stop_token`); колбэки — на главном потоке и только через `lifetime_token` владельца. `res_control_block::then` переводится на эту доставку | Развитие `adapter_worker_base` и статусов `res_control_block` |
| ADR-11 | Изображения во время работы | Runtime image registry (`memory://`-RAM протокол), **handle-индирекция текстур** (id → слот) вместо сырых указателей, заглушка, миниатюры (stb_image_resize2) в `cache://thumbs`, LRU-бюджет GPU/CPU, лимит загрузок в кадр (GL однопоточный) | Расширяет `texture_manager` и `generate_texture` |
| ADR-12 | Сохранения | `save_service` на `user://`: транзакционная запись (уникальный tmp → flush/fsync → rename, ротация `.bak`), future с результатом, ошибка как значение, чтение с откатом на `.bak`. Состав снимка определяет игра | Исправляет `vfs_resolver::save_pending_write` |
| ADR-13 | Сборка игры | Player-исполняемый файл (на базе `gs_loop_service` и `core/main.cpp`) + **cook**: обход зависимостей от стартовой сцены и корней через asset database, валидация (ADR-04/05), копирование в `build/<platform>`, исключение editor-only по флагам ассетов/протоколу `edt://`, отчёт | Использует граф зависимостей из ADR-05 |

> **ADR-03, решение владельца 2026-10-18 (#108):** пакет называется `snake_sdk` (`find_package(snake_sdk)`, таргеты `snk::engine`, `snk::engine_api`), а не «SnakeEngine»; сторонние библиотеки вшиты в SDK; поддерживаются статическая и shared (`SNK_ENGINE_SHARED`) сборки. Реализация: `cmake/SnkInstall.cmake`.

---

## 5. Роадмап по вехам

Номера новых эпиков начинаются с **EPIC-50** (глобальный автоинкремент). Размер: S ≈ до недели, M ≈ 1–3 недели, L ≈ 3–6 недель на одного разработчика; оценки ориентировочные и уточняются в ADR.

### M0 — Фундамент и критичные дефекты (блокирует всё)

Уже сделано в EPIC-49 и EPIC-41 (срезы 1–2): движок конфигурируется из корня, слои через `*_api`, `snk::` алиасы, `depends_on` + топосорт модулей, общий `frame_stage_registry`, развязка scene↔render, `acquire_free_world_id`. Остаток M0:

| Эпик / пункт | Размер | Требования |
|---|---|---|
| **Ручная проверка рендера после EPIC-49** (чек-лист из `docs/agent/handoff/BUILD-STATUS-HANDOFF.md` §3C) — до того как строить поверх | S | — |
| EPIC-41 — **закрыть** US-41-7/8/10 (BL-008) и **добавить** US: install/export, `find_package` config, физический `public/` split (отложенный этап US-49-1) | M | PRJ04, BLD01 |
| EPIC-29 Service Layer — остаток после US-49-6: `res::get_system`, `gui::get_system`, `gs_game_system`, skinning на assembly-time (BL-007), распределение регистраций из `gs_game_init` по модулям | M | DSC01, ADR-01 |
| **EPIC-50 Scheduler & Determinism** — visited-set в `run_graph`, fixed organizer, фазы, clamp dt, seeded RNG; `frame_allocator_reset` в `gs_loop_service` (BL-005); выдача id миров в `edt_world_controller` через `acquire_free_world_id`; `reload_world` не стирает системы других миров | M | RUN03, SCN04 |
| **EPIC-51 Desc Loader Robustness** — без падений на неизвестном/отсутствующем `__type` и отсутствующем `__parent`, детекция циклов, диагностический коллектор (ресурс + JSON-путь + причина), problems-панель | S–M | DSC05, DIA01 |
| Надёжность из бэклога: **BL-002** (release-активный `ENGINE_VERIFY`), **BL-004** (периметр исключений + атомарный bootstrap), **BL-006** (гонки `res_control_block`); EPIC-31 (информативные ассерты) объединить с BL-002 | M | DSC05, TSK01, DIA01 |
| **BL-003** CI (сборка + тесты на PR, MSVC) — нужен для протокола приёмки и воспроизводимости SDK | S | DIA01, PRJ06 |

### M1 — Проект, игровой модуль, player

| Эпик | Размер | Требования |
|---|---|---|
| EPIC-36 `edt://` (как есть) | S | PRJ03, BLD02 |
| **EPIC-42 — переработать** в «Project System»: манифест (ADR-02), create/open/close/recent, слоёный `res://`, `engine://`, `cache://`, `user://`, проверка версии движка | L | PRJ01–03, PRJ05 (частично) |
| **EPIC-52 Game Module API** — загрузка `game`/`game_editor` модулей из проекта (сначала статически в source-сборке, затем dll), `game_module_service`, пример игрового модуля во внешнем каталоге | L | DSC01, DSC06, PRJ04 |
| **EPIC-53 Editor Extension API** — публичные реестры в `app_data_storage`: панели, меню/команды, renderer'ы inspector'а, виды ассетов, превью | M | DSC06, AST01 |
| **EPIC-54 Standalone Player** — исполняемый файл без редактора, запуск стартовой сцены проекта, общий с редактором runtime | M | BLD01, RUN03 |

**Веха:** AC01 проходит в source-сборке (внешний проект, тестовый модуль, стартовая сцена).

### M2 — Данные: схема, идентичность, версии

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-55 Desc Type Registry & Schema** (ADR-04) — схема полей, generic (де)сериализация, мост для старых desc | L | DSC01, DSC03 |
| **EPIC-56 Data Documents & Schema Inspector** — открытие любого `is_document` типа, inspector из схемы (строки, enum, списки/словари с add/remove/reorder, optional, struct), итог vs override, reset, типизированный пикер ссылок | L | DSC02–04 |
| **EPIC-57 Asset Identity & Database** (ADR-05) — GUID, `.meta`, guid↔путь, обратные зависимости, rename/move/delete с отчётом, стабильный `__id` узлов | L | AST02, SCN05 |
| **EPIC-58 Data Versioning & Migration** (ADR-06) | M | DAT01, PRJ05 |
| EPIC-28 Resource Cache tag+type (как есть) | S | предпосылка EPIC-56 |

**Веха:** AC02 (карты, пак, правила создаются, сохраняются и открываются без ручного JSON).

### M3 — Редактор как документный инструмент

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-59 Document Model** — `document`/`document_manager`, документ владеет desc, dirty, своим world-контекстом; save/save all/close с промптом; `shared_state` → документ сцены | L | EDT01, SCN04 |
| EPIC-15 Undo/Redo — **расширить**: стек на документ, save-point для dirty, merge непрерывного drag, команды полей data-документов | M | EDT02 |
| **EPIC-60 Autosave & Recovery** — автосохранение в `cache://`, восстановление после падения, конфликт с внешним изменением файла | M | EDT03 |
| EPIC-25 Prefab Editor — **завершить** (US-25-2…25-7) на основе документной модели и стабильных `__id` | L | SCN02, SCN03 |
| **EPIC-61 Viewport Picking & Incremental Reload** — picking кликом (ID-буфер), точечное обновление ECS вместо полной пересборки мира, выделение по stable id | M | SCN01, SCN05 |
| **EPIC-62 Asset Browser v2 & Import Pipeline** — виды ассетов (из EPIC-53), фильтры, миниатюры, create-действия, импорт PNG с настройками, повторный импорт с сохранением GUID; EPIC-34 входит сюда частично (настройки импорта) | L | AST01, AST03 |
| EPIC-26 / EPIC-27 / EPIC-07 — **урезанный объём**: материал карты (лицо, рубашка, прозрачная рамка), назначение текстур, вывод ошибок шейдеров в problems-панель | M | SCN06 |

**Веха:** AC03 (стол, камера, prefab карты, два экземпляра, overrides, Undo/Redo, сохранение).

### M4 — Runtime: Play, задачи, изображения, сохранения

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-63 Play Mode** (ADR-07) — Play/Stop/Pause/Step, изолированный level, тестовый профиль, 10 циклов без утечек | M–L | RUN01, RUN02, SCN04 |
| **EPIC-64 Task System** (ADR-10) — пул, main-thread очередь, прогресс/отмена, lifetime-токены, панель задач; перевод импорта/экспорта/загрузок; HTTP-клиент (Boost.Beast) как доступный игре сервис | M | TSK01, DIA01 |
| **EPIC-65 Runtime Images & Texture Budget** (ADR-11) | M–L | IMG01 |
| **EPIC-66 Save Service** (ADR-12) | S–M | SAV01 |

M4 можно вести параллельно с M3 после M1; EPIC-64 желательно раньше EPIC-62 (импорт на общей системе задач).

**Веха:** AC05 (игровая сессия, скрещивание с тестовым генератором, ожидание/ошибка/повтор, профиль и источники не тронуты).

### M5 — Игровой UI

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-67 UI Runtime** (ADR-09) — дерево элементов как desc, layout (Yoga + якоря), clipping/scroll, UI render pass, ввод с модальной блокировкой и focus/hover/disabled/drag | L | GUI01, GUI02, GUI05 |
| **EPIC-68 Fonts & Text** — TTF, кириллица/латиница, атлас, перенос строк, многоточие, пересборка под DPI 100/150% | M | GUI03 |
| **EPIC-69 UI Templates, Data Binding & Actions** — шаблоны элементов, привязка к полям схемы, списки/сетки с виртуализацией (1000 карт), реестр игровых действий | L | GUI04, GUI05, IMG01 |
| **EPIC-70 UI Editor** — UI-документ в редакторе: иерархия, inspector, предпросмотр разрешений и DPI, тестовые данные | L | GUI01, GUI02 |

Зависит от M2 (схема, документы) и частично от M4 (изображения). Можно начинать EPIC-67/67 параллельно с M3.

**Веха:** AC04 (коллекция и экран скрещивания из UI-ресурсов, два разрешения, масштаб).

### M6 — Выпуск этапа А

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-71 Cook & Package** (ADR-13) — валидация, обход зависимостей, editor-only исключение, отчёт, standalone в чистом каталоге | M–L | BLD01, BLD02 |
| **EPIC-72 SDK Distribution & Project Upgrade** — версионированная установка, пример игрового расширения, явное обновление проекта с отчётом и бэкапом, проверка «один проект — готовый и локальный редактор» | M | PRJ04–06 |
| **EPIC-73 Diagnostics & Docs** — контекст операции в логах, метрики кадра/памяти/задач в редакторе (EPIC-44 частично), документация платформы/toolchain/восстановления, протокол приёмки AC01–AC06 | M | DIA01 |

**Веха:** AC06 (этап А) + протокол приёмки.

### Этап Б

| Эпик | Размер | Требования |
|---|---|---|
| **EPIC-74 UI Tweens** — move/rotate/scale/alpha, отмена при смене экрана | M | GUI06 |
| **EPIC-75 Audio** — miniaudio, категории громкости, музыка без дублирования после Play/Stop, настройки в `user://` | M | AUD01 |
| **EPIC-76 Localization** — строковые таблицы как desc, выбор языка, подстановки, fallback, диагностика отсутствующих строк, привязка в UI | M | LOC01 |

---

### Эпики вех в GitHub Issues

Задачи ведутся в [GitHub Issues](https://github.com/Prorok1015/OpenGL_Engine/issues); milestones: M1 «Проект и игровой модуль», M2 «Данные», M3 «Документный редактор», M4 «Runtime», M5 «Игровой UI», M6 «Выпуск этапа А», «Этап Б (ТЗ)». Эпики фазы 0 (этапы C/D) — в [ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md) §7. EPIC-50, EPIC-55, EPIC-64, EPIC-29 и EPIC-45 вошли в эпики этапов C/D (#8, #16, #17, #6, #15), отдельных issue для них нет. Манифест: [`docs/agent/tracker/manifest.json`](../agent/tracker/manifest.json).

| Веха | Эпик | Issue |
|---|---|---|
| M1 | EPIC-42 Project System | [#19](https://github.com/Prorok1015/OpenGL_Engine/issues/19) |
| M1 | EPIC-36 `edt://` | [#20](https://github.com/Prorok1015/OpenGL_Engine/issues/20) |
| M1 | EPIC-52 Game Module API | [#21](https://github.com/Prorok1015/OpenGL_Engine/issues/21) |
| M1 | EPIC-53 Editor Extension API | [#22](https://github.com/Prorok1015/OpenGL_Engine/issues/22) |
| M1 | EPIC-54 Standalone Player | [#23](https://github.com/Prorok1015/OpenGL_Engine/issues/23) |
| M2 | EPIC-51 Desc diagnostics | [#24](https://github.com/Prorok1015/OpenGL_Engine/issues/24) |
| M2 | EPIC-28 Resource Cache | [#25](https://github.com/Prorok1015/OpenGL_Engine/issues/25) |
| M2 | EPIC-56 Data Documents & Schema Inspector | [#26](https://github.com/Prorok1015/OpenGL_Engine/issues/26) |
| M2 | EPIC-57 Asset Identity & Database | [#27](https://github.com/Prorok1015/OpenGL_Engine/issues/27) |
| M2 | EPIC-58 Data Versioning & Migration | [#28](https://github.com/Prorok1015/OpenGL_Engine/issues/28) |
| M3 | EPIC-15 Undo/Redo | [#29](https://github.com/Prorok1015/OpenGL_Engine/issues/29) |
| M3 | EPIC-25 Prefab Editor | [#30](https://github.com/Prorok1015/OpenGL_Engine/issues/30) |
| M3 | EPIC-59 Document Model | [#31](https://github.com/Prorok1015/OpenGL_Engine/issues/31) |
| M3 | EPIC-60 Autosave & Recovery | [#32](https://github.com/Prorok1015/OpenGL_Engine/issues/32) |
| M3 | EPIC-61 Viewport Picking & Incremental Reload | [#33](https://github.com/Prorok1015/OpenGL_Engine/issues/33) |
| M3 | EPIC-62 Asset Browser v2 & Import Pipeline | [#34](https://github.com/Prorok1015/OpenGL_Engine/issues/34) |
| M3 | EPIC-07 Material editor | [#35](https://github.com/Prorok1015/OpenGL_Engine/issues/35) |
| M3 | EPIC-26 Shader Desc | [#36](https://github.com/Prorok1015/OpenGL_Engine/issues/36) |
| M3 | EPIC-27 Texture Desc | [#37](https://github.com/Prorok1015/OpenGL_Engine/issues/37) |
| M3 | EPIC-34 Model Importer | [#38](https://github.com/Prorok1015/OpenGL_Engine/issues/38) |
| M4 | EPIC-63 Play Mode | [#39](https://github.com/Prorok1015/OpenGL_Engine/issues/39) |
| M4 | EPIC-65 Runtime Images & Texture Budget | [#40](https://github.com/Prorok1015/OpenGL_Engine/issues/40) |
| M4 | EPIC-66 Save Service | [#41](https://github.com/Prorok1015/OpenGL_Engine/issues/41) |
| M5 | EPIC-67 UI Runtime | [#42](https://github.com/Prorok1015/OpenGL_Engine/issues/42) |
| M5 | EPIC-68 Fonts & Text | [#43](https://github.com/Prorok1015/OpenGL_Engine/issues/43) |
| M5 | EPIC-69 UI Templates, Data Binding & Actions | [#44](https://github.com/Prorok1015/OpenGL_Engine/issues/44) |
| M5 | EPIC-70 UI Editor | [#45](https://github.com/Prorok1015/OpenGL_Engine/issues/45) |
| M6 | EPIC-71 Cook & Package | [#46](https://github.com/Prorok1015/OpenGL_Engine/issues/46) |
| M6 | EPIC-72 SDK & Project Upgrade | [#47](https://github.com/Prorok1015/OpenGL_Engine/issues/47) |
| M6 | EPIC-73 Diagnostics & Docs | [#48](https://github.com/Prorok1015/OpenGL_Engine/issues/48) |
| M6 | EPIC-44 Memory profiling | [#49](https://github.com/Prorok1015/OpenGL_Engine/issues/49) |
| Б | EPIC-74 UI Tweens | [#50](https://github.com/Prorok1015/OpenGL_Engine/issues/50) |
| Б | EPIC-75 Audio | [#51](https://github.com/Prorok1015/OpenGL_Engine/issues/51) |
| Б | EPIC-76 Localization | [#52](https://github.com/Prorok1015/OpenGL_Engine/issues/52) |

## 6. Граф зависимостей (укрупнённо)

```
M0: [EPIC-49 ✔] → EPIC-41 (остаток) ─┬─ EPIC-29 ─┬─ EPIC-50 (scheduler)
                                    │           └─ EPIC-51 (desc robustness)
                                    └─ BL-002/004/006, BL-003 (CI)
             ▼
M1: EPIC-36 → EPIC-42 (project) → EPIC-52 (game module) → EPIC-53 (editor ext API)
                                └→ EPIC-54 (player)
             ▼                                  ▼
M2: EPIC-28 → EPIC-55 (schema) → EPIC-56 (data docs)      M4: EPIC-64 (tasks) → EPIC-65 (images)
              EPIC-57 (identity) ─┐  EPIC-58 (versions)        EPIC-63 (play) ← EPIC-50, EPIC-54
             ▼                    │                            EPIC-66 (save)
M3: EPIC-59 (documents) → EPIC-15 (undo) → EPIC-25 (prefab) ← EPIC-57
    EPIC-60 (autosave), EPIC-61 (picking), EPIC-62 (assets) ← EPIC-53, EPIC-57, EPIC-64
    EPIC-26/27/07 (материалы, урезанно)
             ▼
M5: EPIC-67 (UI runtime) → EPIC-69 (binding) → EPIC-70 (UI editor) ← EPIC-56, EPIC-59, EPIC-65
    EPIC-68 (fonts)
             ▼
M6: EPIC-71 (cook) ← EPIC-57, EPIC-54 · EPIC-72 (SDK) ← EPIC-41, EPIC-52 · EPIC-73
             ▼
Б:  EPIC-74, EPIC-75, EPIC-76
```

Критический путь: **EPIC-41 (остаток) → 29 → 42 → 52 → 55 → 56/57 → 59 → 67/69 → 70 → 71**.

---

## 7. Существующие эпики: что с ними делать

| Эпик | Решение |
|---|---|
| EPIC-49 | Done. Использовать как базу (модули, стадии кадра, api-таргеты); отложенный `public/` split перенести в EPIC-41 |
| EPIC-41, EPIC-29, EPIC-36, EPIC-28 | В работу в M0–M2; EPIC-41 закрыть остаток и расширить install/export |
| Бэклог BL-002…BL-008 | BL-002/004/005/006/003 — в M0 (BL-005 входит в EPIC-50); BL-007 — в EPIC-29; BL-008 — EPIC-41; BL-001 — попутно в EPIC-50 или M3 |
| EPIC-42 | Переработать: проект вне репо движка, манифест ADR-02, разделение каталогов, версия |
| EPIC-15, EPIC-25 | Расширить/завершить в M3 поверх документной модели |
| EPIC-26, EPIC-27, EPIC-07 | Урезать до SCN06 (материал карты, ошибки шейдеров) |
| EPIC-34, EPIC-35 | Взять только настройки импорта и повторный импорт (в EPIC-62); остальное заморозить |
| EPIC-44, EPIC-46 | Частично в EPIC-73 (метрики памяти); Tracy — по необходимости |
| EPIC-31 | Желательно в M0 — информативные ассерты ускоряют всю работу |
| EPIC-33, EPIC-48, EPIC-45 | **Заморозить**: скелетная анимация и оптимизация 3D-рендера не нужны карточной игре этапа А. EPIC-33 закрыть по уже сделанному |
| EPIC-32, EPIC-40, EPIC-47 | Заморозить (низкий приоритет; EPIC-47 не даёт устойчивой идентичности) |

---

## 8. Риски

| Риск | Митигация |
|---|---|
| Рендер-пути после EPIC-49 не покрыты тестами | Ручная проверка по чек-листу handoff до старта M1; визуальные smoke-тесты (скриншот-сравнение) в CI позже |
| ABI C++ через границу dll, разные toolchain у игры и SDK | Закрепить компилятор/версию в SDK, проверка при загрузке модуля (версия SDK + хеш ABI), source-путь как запасной |
| Глобальные синглтоны дублируются в dll | EPIC-29 до EPIC-52; движок как shared library |
| Миграция рукописных desc на схему затягивается | Мост: старые desc работают как есть, схема обязательна только для новых и игровых типов |
| Полная пересборка мира на каждую правку тормозит на сложных сценах | EPIC-61 (точечный patch через уже существующий `patch` в `register_desc_spawner`) |
| Свой UI-фреймворк — большой объём | Жёсткий минимум виджетов под AC04; Yoga для layout; RmlUi как оценённая альтернатива в ADR-09 |
| GL однопоточный — подгрузка 1000 изображений даёт фризы | Бюджет загрузок в кадр, миниатюры, виртуализация списков |
| ТЗ v0.1 не согласовано окончательно | Каждый эпик ссылается на ID требований; при изменении ID объём пересматривается точечно |

---

## 9. Вопросы к заказчику

1. Целевая ОС и минимальное железо этапа А. Предполагаем Windows 10/11 x64 и MSVC (VS 18) — это и упоминание масштаба Windows в ТЗ, и фактическая среда разработки движка. Нужен ли Linux/macOS?
2. Допустимо ли требование к игровым C++-расширениям «тот же компилятор и версия, что в SDK»?
3. Генератор изображений — внешний HTTP-сервис? Нужны ли от движка HTTP-клиент и хранение ключей вне сборки (BLD02 «секреты»)?
4. Бюджеты: время открытия проекта, память на коллекцию 1000 карт, время сборки — какие числа принимаем для приёмки?
5. Визуальный стиль UI: нужны ли SDF-шрифты, 9-slice, эффекты (тени, градиенты) на этапе А?
6. Порядок вех: подтверждаете ли приоритет «данные → документы → UI» над Play, или Play нужен раньше для проверки геймплейного кода?
