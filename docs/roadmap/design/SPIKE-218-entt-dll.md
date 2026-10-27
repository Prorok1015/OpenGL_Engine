# Спайк #218: EnTT и регистрация компонентов/систем из отдельной DLL

**Issue:** #218 (S1 эпика #217; блокирует DLL-часть #21 EPIC-52, #220, #222) · **PR:** #387 (ветка `wip/218-dll-spike`, «[spike, do not merge]») · **Дата:** 2026-10-27

Вопрос спайка: можно ли безопасно регистрировать компоненты, системы и desc-типы из динамически загружаемого модуля (Windows DLL с MSVC, Linux `.so`) и что для этого нужно в движке. Ответ получен кодом и проверен в CI, а не выведен рассуждением.

## 1. Что сделано

**Прототип** (`tests/module_dll/`, подключён только при `SNK_ENGINE_SHARED`, то есть в CI-джобах Linux GCC Shared (Debug) и Windows MSVC Shared Release):
- `spike_game` — SHARED-библиотека, линкуется с `snk_engine` как будущий `game.dll`, видимость hidden (как у всех таргетов проекта). Содержимое:
  - `SNK_DECLARE_MODULE(spike_game_module)`;
  - компонент `spike::velocity_component`;
  - desc `spike_velocity_desc` со спавнером через `scn::registration_scope`;
  - система `spike::move_system`, которая пишет в компонент движка `scn::local_transform`;
  - meta через `ecs::register_component`;
  - C-функции для проб: `spike_probe`, `spike_spawn_in_sandbox`, `spike_emplace_module_only`, `spike_resolve_engine_type`.
- `app_module_dll_tests.cpp` (в `unit_tests`). Хост делает следующее:
  1. Грузит модуль через `dlopen`/`LoadLibraryW` и проверяет `check_module_entry`.
  2. Передаёт сервисы (`desc_system`, `ecs_assembler`, `system_factory`) через `ds::app_data_storage`.
  3. Сравнивает идентификаторы типов по обе стороны границы.
  4. Гоняет три пути:
     - путь песочницы `scn::world_context::apply_commands`: в нём движок вызывает `entt::resolve(id)` и meta-функцию модуля;
     - сборку префаба ассемблером: desc модуля → спавнер модуля;
     - план систем: система модуля плюс система хоста, которая читает компонент модуля.
  5. Отцепляет данные модуля, выгружает модуль и загружает заново (два цикла).
  6. Принадлежность адреса модулю проверяет через `dladdr` / `GetModuleHandleExW(FROM_ADDRESS)`: vptr хранилищ, `type_info`, meta-узлы.

**Изменения движка** (минимальные, предлагаются в #220 как есть):
- `ecs::meta_context()` и `ecs::meta_context_handle()` (`SNK_API`, `core/core/ecs/ecs_meta_context.cpp`) — meta-контекст `snk_engine`, к которому подключаются другие бинарники.
- `ecs::register_component<T>(entt::meta_ctx&)` через `entt::meta_factory<T>`. Прежняя версия звала `entt::meta<T>()`, который в EnTT 3.15 помечен `[[deprecated]]`, поэтому под `-Werror` её нельзя было даже инстанцировать. Функцию до сих пор никто не вызывал.
- Защиты: `#error` при `ENTT_STANDARD_CPP` и `static_assert(entt::id_type == std::uint32_t)` в `ecs_component.h`; проверка в CMake (корневой `CMakeLists.txt`) на `ENTT_STANDARD_CPP` и `ENTT_ID_TYPE` во флагах, определениях каталога и интерфейсе `EnTT`.
- Модулю на MSVC нужна своя статическая копия Boost.JSON (`tests/module_dll/CMakeLists.txt`), как у `editor_system`: `snk_engine.dll` экспортирует только `SNK_API`.

## 2. Результаты CI

| Прогон | Что | Linux GCC Shared | Windows MSVC Shared Release |
|---|---|---|---|
| [40506186217](https://github.com/Prorok1015/OpenGL_Engine/actions/runs/40506186217) | первый пуш | 1 проверка: моя ошибка в ожидании (см. §3.3) | LNK2019 на `boost::json::object::operator[]`/`if_contains` в модуле |
| [40512619751](https://github.com/Prorok1015/OpenGL_Engine/actions/runs/40512619751) | **базовый прототип** | ✅ все проверки | ✅ все проверки (зелёный весь full tier) |
| [40518645629](https://github.com/Prorok1015/OpenGL_Engine/actions/runs/40518645629) | эксп. 1: `ENTT_STANDARD_CPP` только в модуле | ❌ `type_hash` модуля = 0/1, `type_name` пуст; SIGABRT на `ENTT_ASSERT("Invalid key")` в `ctx().get<entity_spawner>()` | ❌ те же хеши 0/1; access violation |
| [40520701077](https://github.com/Prorok1015/OpenGL_Engine/actions/runs/40520701077) | эксп. 2: `ENTT_ID_TYPE=std::uint64_t` только в модуле | ❌ мусор в общих структурах (сдвинулась раскладка `module_probe`), SIGABRT, SIGSEGV | ❌ access violation, segfault процесса |
| [40521901085](https://github.com/Prorok1015/OpenGL_Engine/actions/runs/40521901085) | эксп. 3: `ENTT_API_IMPORT` только в модуле | ✅ тесты проходят, но `type_index` по-прежнему свой, и **образ остаётся загруженным после `dlclose`** | ❌ LNK2019: `__imp_entt::type_index<spike::velocity_component>::value` и ещё 6 типов |

Эксперименты 1–3 откачены. Защиты из §1 переводят ошибки 1 и 2 из рантайма в compile/configure time.

## 3. Факты

### 3.1. Идентификаторы типов
- **`entt::type_hash<T>` совпадает** у хоста и модуля на обоих компиляторах: он `constexpr` от `__PRETTY_FUNCTION__`/`__FUNCSIG__`. Между компиляторами значения разные: `velocity_component` — 1035557170 у GCC и 1032846185 у MSVC, потому что `__FUNCSIG__` даёт `struct spike::velocity_component`. Поэтому `type_hash` нельзя сохранять в файлы. Desc и так хранят имена типов.
- **`entt::type_index<T>` у каждого бинарника свой**: у модуля velocity = 0, у хоста 65 на Linux и 54 на Windows. Внутри EnTT 3.15 `type_index` используется только в `type_info::index()` и `operator<` (`core/type_info.hpp:202`). Registry, хранилища, `ctx()`, meta и `any` сравнивают типы через `hash()`. В коде движка `type_index` не используется. **Вывод:** разные `type_index` ничего не ломают, `ENTT_API` не нужен.
- **`entt::type_id<T>()`** — функционально-локальная статика, у каждого бинарника своя копия, сравнивается по хешу. Указатели на неё лежат в `basic_sparse_set::info` (`entity/sparse_set.hpp:434`) и в meta-узлах, то есть ведут в код того бинарника, который создал объект.
- **`ds::type_id`** (FNV-1a от `source_location::function_name`) совпадает. `app_data_storage` находит сервисы хоста из модуля.
- **`ENTT_STANDARD_CPP`** превращает `type_hash` в `type_index`. Хранилища, `ctx()` и meta перестают находиться, процесс падает. `#pragma detect_mismatch("entt.nonstd")` в EnTT проверяет только одну линковку, на границе DLL рассогласования не видит (эксп. 1).
- **`ENTT_ID_TYPE`** — часть ABI (раскладка `dense_map`, registry, meta). Отличие только в модуле означает UB и падение (эксп. 2).

### 3.2. Хранилища registry
- Пул ищется по `type_hash`. Компонент, который создал модуль (спавнер, `meta_loader_get`, прямой `emplace`), хост читает и пишет через тот же пул; верно и обратное. Компонент движка (`local_transform`) система модуля меняет в том же пуле.
- **Пул создаёт тот, кто первым вызвал `assure<T>()`.** Его vtable, `type_info*` и control block `shared_ptr` живут в этом бинарнике. Для типов игры это почти всегда модуль: тест проверяет, что vptr пула `velocity_component` лежит в образе модуля.
- После выгрузки модуля такой пул висит. Любой проход по пулам (`destroy`, `clear`, `reset`, деструктор registry) уйдёт в выгруженный код. Это касается и песочницы (`ecs::entity_spawner` в `ctx()`): `apply_commands` чистит сущности, но не пулы.
- **Отцепление работает:** `registry.reset(id)` для пулов модуля до выгрузки (в тесте пул модуля находится по vptr). После этого мир живёт дальше: сущность с компонентом движка цела, `destroy` безопасен (цикл выгрузки и загрузки ×2).

### 3.3. Meta
- **`entt::locator<meta_ctx>` свой у каждого бинарника, включая `unit_tests` относительно `snk_engine`** (hidden visibility на Linux, DLL на Windows). Значит, «передать хендл хоста», как предлагал EPIC-29 §3 (`docs/roadmap/design/EPIC-29_SERVICES_DLL.md:28`), неточно. Подключаться надо к контексту **движка**: `entt::resolve(id)` вызывается из `snk_engine` (`scn_world_context.cpp:115`). Это и делают `ecs::meta_context()`/`meta_context_handle()`.
- Без подключения meta модуля для движка невидима. `apply_commands` переносит сущность из песочницы без компонента: пул `velocity_component` в мире не появляется. С подключением (`entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle())` в модуле) движок находит тип и вызывает `loader_get` модуля, компонент переносится.
- **`entt::resolve<T>()` для незарегистрированного типа ничего не вставляет в контекст** (`meta/node.hpp:257`). Он строит узел в вызывающем бинарнике и возвращает по значению. Моё исходное ожидание («неявные узлы засоряют контекст») CI опроверг (прогон 40506186217). В контексте остаются только регистрации `meta_factory`, и их снимает `entt::meta_reset<T>(ctx)` модуля. Висеть после выгрузки может `meta_type`/`meta_any`, если хост хранит его как значение.
- В тесте есть страховка: id meta-типов, появившихся за время жизни модуля, сбрасываются при выгрузке. Она ничего не нашла: после `shutdown_services` в контексте 0 узлов модуля.

### 3.4. Наши реестры
| Что | Где | Состояние | Что держит код модуля |
|---|---|---|---|
| `system_factory` / `system_registry` | `ecs_system.h`, `ecs_system_registry.hpp:64` | объект хоста, не static | `run` (указатель на функцию), `std::function make`; **`remove` нет**, повторная регистрация того же имени — ASSERT |
| `system_plan` | `ecs_system_plan.hpp` | в мире/контексте | копии `run`/`make`, экземпляры stateful-систем (vtable модуля) |
| `access_entry` | `ecs_system_access.hpp:29` | в записи системы | `type_name` (`string_view` в rodata модуля) |
| `desc_system` | `desc_system.h:384` | объект хоста | фабрики desc (лямбды модуля), схемы; снимаются `registration_scope` ✅ |
| `ecs_assembler` | `scn_ecs_assembler.h:67` | объект хоста | спавнеры/патчеры; снимаются `registration_scope` ✅ |
| кэш ресурсов | `res::resource_system` | объект хоста | **экземпляры desc типов модуля** (vtable модуля), если они загружены как ресурсы |
| `app_data_storage` | `ds_store.hpp:99` | объект хоста | `entry::name` — `string_view` в rodata регистрирующего бинарника, deleter `shared_ptr`; модуль не должен класть в хранилище хоста то, что переживёт его |
| пулы registry / песочницы | §3.2 | в мирах | vtable, `type_info*`, control block |
| meta | §3.3 | контекст движка | узлы `meta_factory` (`type_info*`, функции) |
| `ds::type_id`, `type_hash` | — | constexpr | ничего |

Синглтоны движка (`config_manager`, логгер, кадровый аллокатор) в shared-сборке живут в одном экземпляре (DISTRIBUTION §1 п.5). Модуль пользуется ими через `SNK_API` и проблем не создаёт.

### 3.5. Выгрузка
Рабочий порядок (тест, обе платформы, 2 цикла, `dlclose`/`FreeLibrary` успешны, образ выгружен):
1. Пулы типов модуля во всех мирах и песочницах: `reset(id)` (или пересборка миров из снимка, DISTRIBUTION §2 п.4).
2. `module->shutdown_services`: `registration_scope` снимает desc/спавнеры/миграции, `meta_reset` типов модуля, `locator::reset()` в модуле.
3. Системы: удалить записи из `system_registry` и пересобрать планы (`rebuild_all_plans`). В спайке это заменено уничтожением `system_factory`: `remove` нет.
4. Ресурсы с desc типов модуля — выбросить из кэша. В спайке уничтожается весь `scene_env`.
5. `snk_destroy_module`, затем `FreeLibrary`/`dlclose`.

**Linux: видимость решает, выгрузится ли образ.** С `ENTT_API_IMPORT` (то есть `visibility("default")` на `type_index`) статики шаблонов становятся `STB_GNU_UNIQUE`, и `dlclose` образ **не выгружает** (эксп. 3: «image still mapped after unload true»). Hot reload на Linux возможен только при hidden visibility модуля. Шаблон игры (#160) обязан ставить `CXX_VISIBILITY_PRESET hidden` / `VISIBILITY_INLINES_HIDDEN`, как `snk_target_defaults`.

### 3.6. Линковка и экспорт
- Модуль линкует `snk::engine` + `snk::engine_api` и больше ничего (на MSVC ещё `Boost::json`). Всё, что вызывает прототип, уже экспортировано: `desc_system`, `ecs_assembler`, `registration_scope`, `build_system_plan`/`system_plan`, `world_context`, `assemble_prefab`, логгер, ассерты. `system_factory` — header-only.
- Новый экспорт нужен один: `ecs::meta_context_handle()` (`meta_context()` — для тестов и редактора).
- Boost.JSON на MSVC не экспортируется: модуль держит свою копию. Это безопасно при общем `/MD` CRT (так уже работает редактор). Аллокация идёт через `operator new` одного CRT. Альтернатива — экспортировать Boost.JSON из `snk_engine` (`BOOST_JSON_DYN_LINK`), решение в #219/#109.
- `check_module_entry` не видит рассогласования `ENTT_*` (эксп. 1–2 падают в рантайме, а не при загрузке).

## 4. Решение (критерии #218)

- **Идентификатор компонента C++ из DLL — `entt::type_hash`** (pretty-function). Он стабилен между бинарниками одного компилятора, а одинаковое семейство компилятора и так обеспечивает `check_module_entry`. `ENTT_API` и собственный `type_info` **не нужны**: `type_index` ни на что не влияет (§3.1), а `ENTT_API` на MSVC требует экспорта каждого `type_index<T>` (эксп. 3).
- **`ENTT_STANDARD_CPP` запрещён, `ENTT_ID_TYPE` не переопределяется**: `#error`/`static_assert` в `ecs_component.h` и проверка в CMake (сделано).
- **Meta — один контекст на процесс, контекст `snk_engine`.** Каждый бинарник (модуль, а при использовании meta и редактор) подключается через `ecs::meta_context_handle()`.
- **Регистрация по стабильному имени через слой, не зависящий от источника кода** — работа #220. Имя системы и имя desc-типа уже стабильны. Для компонентов: компонент с C++-типом регистрируется по имени, а внутри хранилище адресуется `type_hash<T>`. Компонент без C++-типа (wasm/схема, DISTRIBUTION §7.3) получает id `entt::hashed_string{stable_name}` и runtime-хранилище по описанию. Сохранённые данные ссылаются только на имена (`type_hash` разный у MSVC и GCC).

## 5. Рекомендация для EPIC-52 (#21)

**DLL можно делать сразу, без промежуточной «только статики».** В shared-сборке прототип проходит весь путь EPIC-52 на обеих платформах. При этом сначала реализовать слой регистрации (#220) и проверить его на модуле, вкомпилированном статически, во всех CI-конфигурациях. Тот же модуль как DLL — в shared-конфигурациях. Отдельный «статический режим» как продукт не нужен: источник модуля — деталь `module_source`.

Что нужно изменить в регистрации (вход в #220):
1. **Подключение meta автоматически:** `SNK_DECLARE_MODULE` в `snk_create_module` делает `entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle())`, в `snk_destroy_module` — `reset()`. Автор модуля об этом не думает.
2. **`system_registry::remove(name)` + `ecs::system_scope`**: снимает системы модуля, затем `rebuild_all_plans`.
3. **`component_scope`**: регистрирует компонент (имя → `type_hash`, meta в контексте движка), а при уничтожении делает `meta_reset` и **`reset(id)` пулов во всех мирах и песочницах всех контекстов**. Альтернатива — снимок мира в desc v3 и пересборка (DISTRIBUTION §2 п.4–7); для hot reload нужны оба. Список id даёт сам scope, поиск по vptr из спайка — только для тестов.
4. **Ресурсы:** выброс из кэша desc типов модуля до выгрузки (по списку типов из scope).
5. **`build_info`:** добавить флаги EnTT (nonstd, `sizeof(entt::id_type)`) — модуль с другим режимом отклоняется при загрузке, а не падает.
6. **`native_library_source`** = класс `shared_library` из теста (`LoadLibraryW`/`dlopen`, без типов Win32 в публичном заголовке) плюс теневая копия для hot reload (#222).
7. **Шаблон игры (#160):** hidden visibility, `/MD`, Boost.JSON (своя копия на MSVC или экспорт из движка).

API для модуля: всё перечисленное в §3.6 плюс `registration_context` из #220 (системы, компоненты, desc/спавнеры/миграции, игровые фазы). Модуль не трогает реестры напрямую.

## 6. Ограничения спайка
- Проверены Windows MSVC **Release** shared и Linux GCC **Debug** shared. MSVC Debug (`/MDd`) и DebugGame (`/Od /MD`) — в #219.
- Стабильность `__FUNCSIG__` между **разными версиями** MSVC 19.x не проверялась: игра и движок здесь собраны одним toolset. Если формат меняется между минорами, `type_hash` разойдётся. Проверка — в #219 (модуль, собранный другим toolset); страховка — п.5 из §5 расширить на версию MSVC.
- Stateful-системы модуля, подписки на события (`ecs::event`), обработчики сигналов registry (`on_construct` с лямбдами модуля), `thread_local` в модуле и задачи `task_system` с кодом модуля не проверялись. Они держат код модуля так же, как пулы, и должны сниматься тем же scope.
- Перенос состояния мира через выгрузку (снимок) не делался: мир переживает выгрузку, но данные компонентов модуля теряются.

## 7. Оценка трудоёмкости
| Story | Оценка | Комментарий |
|---|---|---|
| #219 Конфигурации поставки, пин MSVC, PDB | M (2–3 дня) | Спайк не меняет. Добавить проверку `type_hash` между toolset'ами (модуль старше/новее) и решение по Boost.JSON (своя копия или экспорт) |
| #220 Слой регистрации | L (6–8 дней) | П.1–5 из §5, перенос тестов спайка в постоянные (статический модуль во всех конфигурациях, DLL — в shared); компоненты по схеме без C++-типа — ещё M (3–4 дня) после #16 |
| #221 Поиск тулчейна и сборка из редактора | M–L (4–6 дней) | Спайк не меняет; зависит от #160, #139, #24 |
| #21 (загрузка DLL) | S (1–2 дня) поверх #220 | `native_library_source` из теста + `module_loader` |
