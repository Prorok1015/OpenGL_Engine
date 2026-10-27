# План: EPIC-42 «Project System» — проект вне репозитория движка

**Issue:** #19 (истории #153–#159; #160 перенесена в #217) · **Основа:** ADR-02 (`docs/roadmap/CHIMERA_ROADMAP.md:124`), ADR-07 (`:129`), ADR-12 (`:134`), веха M1 (`:159-169`), `docs/epics/editor/EPIC-42.md`, `docs/roadmap/design/DISTRIBUTION.md` (#217) · **Статус:** черновик, ждёт решения владельца (раздел 4). Код не пишется до утверждения.

Ссылки `file:line` даны по `aibranch` @ `e47e75b`.

---

## 1. Текущее состояние

### 1.1. Ресурсы и протоколы

- **`vfs_resolver` уже слоёный.** Конструктор кладёт корни в `std::stack` в порядке списка, последний — верхний (`core/core/resource/resolvers/res_vfs_resolver.cpp:22-25`). `resolve_tag` идёт сверху вниз, побеждает первый существующий файл (`:292-314`). `store()` пишет только в верхний слой (`:138-167`). `exists()` проверяет все слои (`:226-244`). Watcher подписан на все корни (`:22-27`), изменение в нижнем слое глушится, если выше лежит файл с тем же путём (`is_shadowed`, `:79-103`).
- **`safe_path`** — единственное место, где тег становится путём: отвергает `..`, корни, буквы дисков, управляющие символы и выход за корень после `weakly_canonical` (`:246-290`). Открытый PR #377 меняет его: канонизируется только родительский каталог, симлинк в последнем компоненте отвергается. План опирается на версию после #377.
- **Только чтение пока не выразить.** `store()` у `vfs_resolver` всегда пишет в верх; флага read-only нет. `erase()` разрешён только протоколу `memory` (`:169-175`).
- **Корни берутся из конфига внутри конструктора `resource_system`.** `resource/path` (по умолчанию `./res`) и `resource/base_path` — глобальные cfg-переменные (`core/core/resource/res_system.cpp:18-20`); конструктор читает их и регистрирует `res://` с двумя слоями (`:194-211`). Слои — 2 после #176: `core/res` (база) + `Editor/res` (верх).
- **`memory://`** лежит в `<resource/path>/../sandbox` с `// TODO: make configurable` (`res_system.cpp:213-216`). Для проекта это `<project>/sandbox`, рядом с контентом.
- **Список слоёв — статическая функция** над cfg (`res_system.cpp:805-811`, объявление `res_system.h:68`). Её зовут редактор: Asset Browser (`Editor/code/editor_system/controller/edt_editor_system.cpp:270`) и команды состояния для MCP (`Editor/code/editor_system/core/edt_editor_context.cpp:29`, `edt_state_commands.cpp:862`).
- **Движковый код ссылается на `res://`**: шейдеры пассов (`core/engine/render/render_system/passes/rnd_composition_pass.cpp:102-103`, `rnd_z_prepass.cpp:60-61`, `rnd_debug_pass.cpp:39-40`). Редактор — на `res://templates/…` и `res://icons/…` (`edt_level_controller.cpp:170`, `edt_prefab_document.cpp:47`, `edt_world_controller.cpp:100`, `edt_editor_system.cpp:101-102`, `Editor/code/editor_system/core/editor_module.cpp:39`). Перевод редакторных ссылок на `edt://` — EPIC-36 (#162).
- **Содержимое.** `core/res` (3.0 МБ): `base_*.desc`, `shaders/`, `skybox/`. `Editor/res` (4.1 МБ): `bench/`, `icons/`, `levels/` (в т.ч. `smoke_level.desc`, `smoke_two_worlds.desc` для CI), `objects/robot`, `samples/`, `shaders/templates/`, `templates/`. Игровой, тестовый и редакторный контент смешаны.

### 1.2. Конфиг и запуск

- `main` сначала разбирает CLI и конфиг, потом добавляет модули `core → engine → editor` (`Editor/main.cpp:46-55`). `resource_system` создаётся в `core_module::register_services` через `res::resource_init` (`core/core/core_module.cpp:83`, `core/core/resource/res_resource_service_init.cpp:5`).
- CLI: `--config <file>` и `--set name value …` (`core/core/config/cfg_api.cpp:97-138`). Путь-переменная из файла считается относительно каталога этого файла (`cfg_api.h:41-42`: `location / val`), из `--set` — относительно рабочего каталога (`cfg_api.cpp:131`). Есть `extends:` (`cfg_api.cpp:13,74-80`).
- `po::parse_command_line` отвергает неизвестные опции, поэтому `--project` нужно объявить в том же `options_description`. Попутно: `help` и `config` оба объявлены с коротким именем `-c` (`cfg_api.cpp:100-101`); заведён bug #382.
- `Editor/config/editor.cfg:1-3`: `base_path: ../../core/res/`, `path: ../res/`, то есть раскладка репозитория.
- Заголовок окна жёстко «Snake Editor» (`editor_module.cpp:46`).
- MCP: `editor/project_dir` (пусто → рабочий каталог) задаёт, куда писать `.snk/mcp.json` (`edt_editor_system.cpp:59-62,257-258`; `edt_agent_server.cpp:37`).
- Стартовый уровень: `editor/startup_level` (`edt_editor_system.cpp:53-54,248`). Меню File: `file.new/open/save/save_as/import_model/exit` (`Editor/code/editor_system/controller/edt_file_menu.cpp:189-238`).
- ImGui `.ini` отключён (`core/engine/gui/backends/opengl/gui_gl_backend.cpp:21`); места для пользовательских настроек редактора нет.

### 1.3. Версия движка и игровой модуль

- Версии движка в рантайме нет. `SNK_SDK_VERSION 0.2.0` существует только в CMake (`cmake/SnkInstall.cmake:29,298`). У модулей своя версия API (`core/core/application/include/snk/application/app_module_entry.h:101-144`), она про ABI, а не про формат проекта.
- `game_module_service` из старого EPIC-42.md в коде нет (grep пуст). DLL с диска не грузится: это #21 (EPIC-52), блокер — спайк #218 (`DISTRIBUTION.md` §1, §4).

### 1.4. CI, упаковка, SDK

- Тесты знают пути репозитория: `SNK_EDITOR_RES_DIR`, `SNK_CORE_RES_DIR` (`CMakeLists.txt:132-133`) в `tests/null_engine_fixture.hpp:41` и `tests/desc_repo_roundtrip_tests.cpp:16-37`.
- Гейт `desc_resave --check`: `core/res` и `Editor/res --base core/res` (`.github/workflows/build.yml:408-412`).
- Visual smoke: `tools/run_headless.sh` по умолчанию передаёт `--config Editor/config/editor.cfg` (`tools/run_headless.sh:37,104`), уровни задаются `--set editor/startup_level levels/…` (`build.yml:471-512`); эталонные PNG меняет только владелец.
- `release.yml` копирует `core/res` → `res_core/`, `Editor/res` → `res/` и переписывает `base_path` sed'ом (`.github/workflows/release.yml:57-79`).
- SDK (`cmake/SnkInstall.cmake`) ставит заголовки и библиотеки, ресурсов не ставит (`:170-185`). `tests/package_consumer` ресурсы не использует.

### 1.5. Что мешает «проекту вне репозитория движка»

1. Нет понятия проекта: корень контента — это cfg-переменная, и путь к ней в `editor.cfg` записан относительно раскладки репозитория.
2. `resource_system` берёт корни из глобального конфига прямо в конструкторе; список слоёв — статика. Подставить корни проекта можно только через `--set`, а изолировать нельзя: `memory://` попадает в `<project>/sandbox`.
3. `Editor/res` смешивает редакторное (→ `edt://`, EPIC-36), тестовые уровни CI и игровой контент. Убрать игровой контент из репозитория движка нельзя, пока на него смотрят тесты, гейт desc и smoke.
4. Нет версии движка в рантайме, значит нечего сверять с `engine_version`.
5. CLI не знает `--project`, заголовок окна не знает имени проекта, Recent Projects негде хранить.
6. `cache://`, `user://`, `engine://` нет; каталог `build` нигде не определён.

---

## 2. Целевая модель

### 2.1. Манифест `project.snkproj`

Формат — JSON (решено в ADR-02). Разбор — Boost.JSON (уже в зависимостях `snk::resource_system`, `core/core/resource/CMakeLists.txt:10`).

```json
{
  "format_version": 1,
  "name": "Sandbox",
  "engine_version": "0.2.0",
  "start_scene": "res://levels/smoke_level.desc",
  "content_roots": ["res"],
  "modules": [
    { "name": "game", "kind": "runtime" },
    { "name": "game_editor", "kind": "editor", "optional": true }
  ],
  "platforms": ["windows-x64"]
}
```

| Поле | Тип | Обяз. | Смысл |
|---|---|---|---|
| `format_version` | int | да | Версия формата манифеста. Больше поддерживаемой → ошибка открытия. Меньше → открытие без миграции (миграции — явная команда, ADR-06) |
| `name` | string | да | Заголовок окна, Recent, имя каталога `user://` |
| `engine_version` | string `MAJOR.MINOR.PATCH` | да | Сверяется с версией редактора (§2.6) |
| `start_scene` | `res://`-тег | нет | Уровень при открытии; `editor/startup_level` из `--set` его перекрывает |
| `content_roots` | string[] относительно корня проекта | нет, по умолчанию `["res"]` | Слои `res://` проекта; первый в списке — самый приоритетный и принимает запись |
| `modules` | `{name, kind: runtime\|editor, optional}`[] | нет | Игровые модули (§2.7, #159) |
| `platforms` | string[] | нет | Пока только `windows-x64` (DISTRIBUTION §3.3); в M1 только хранится |

Правила: неизвестное поле — предупреждение в лог (вперёд-совместимость); неверный тип или отсутствие обязательного поля — ошибка с JSON-путём; пути в `content_roots` проходят ту же проверку, что `safe_path` (без `..`, абсолютных путей и букв дисков).

API (новый модуль `snk::project`, `core/core/project/`, namespace `prj`):

```cpp
namespace prj {
	struct module_ref { std::string name; module_kind kind; bool optional = false; };
	struct manifest {
		int format_version; std::string name; engine_version engine; std::optional<res::tag> start_scene;
		std::vector<std::filesystem::path> content_roots; std::vector<module_ref> modules; std::vector<std::string> platforms;
	};
	struct load_error { std::string json_path; std::string reason; };
	SNK_API std::expected<manifest, load_error> parse_manifest(std::string_view json);
	SNK_API std::expected<manifest, load_error> load_manifest(const std::filesystem::path& file);

	// Каталоги открытого проекта; кладётся в app_data_storage до resource_system.
	struct project_context { std::filesystem::path root; manifest data; std::filesystem::path cache_dir, user_dir, build_dir; };
}
```

Чтение файла манифеста идёт до `resource_system` (он ещё не создан и ему нужны корни из манифеста), поэтому это прямое чтение с диска в одной функции `load_manifest`, а не через кэш ресурсов. Это единственное такое место; дальше манифест берут из `project_context`.

### 2.2. Раскладка проекта

```
<project>/                      ← где угодно, вне репозитория движка
├── project.snkproj
├── res/                        ← res:// (верхний слой), сюда пишет store()
├── .cache/                     ← cache:// — регенерируемое, в .gitignore
│   ├── memory/                 ← memory:// (было <res>/../sandbox)
│   └── user_profile/           ← user:// в редакторе (тестовый профиль, §2.4)
├── .snk/                       ← mcp.json, agent_policy.json (AI_INTEGRATION.md, уже есть)
├── build/                      ← сборка игрового кода (#160/#217), не протокол
└── code/                       ← исходники game-модуля (шаблон #160)
```

Установка движка (репозиторий или `Snake/<ver>/` из ADR-03) даёт два корня: база движка (`core/res` в репо, `res_core/` в zip) и данные редактора (`Editor/data`, EPIC-36).

### 2.3. Протоколы

| Протокол | Корни (снизу вверх) | Запись | Кто регистрирует |
|---|---|---|---|
| `res://` | база движка → `content_roots` проекта (в обратном порядке списка) | верхний слой проекта | `resource_system` из таблицы монтирования |
| `engine://` | только база движка | нет (read-only) | `resource_system` |
| `edt://` | `Editor/data` | `edt://settings/` — см. вопрос 9 | редактор (EPIC-36, #161) |
| `cache://` | `<project>/.cache` | да | `resource_system` |
| `user://` | редактор: `<project>/.cache/user_profile`; player: ОС-каталог (§2.4) | да | `resource_system` |
| `memory://` | `<project>/.cache/memory` | да, единственный с `erase()` | `resource_system` |

Как это ложится на код:

- **Таблица монтирования вместо cfg-статик.** Новый `res::mount_table { std::vector<mount> }`, `mount { protocol, roots (снизу вверх), writable }`. `resource_system(tasks, mount_table)`. `resource_init` строит таблицу из `prj::project_context`, если он есть в хранилище, иначе из `resource/base_path` + `resource/path`, как сейчас (режим без проекта, вопрос 1). Статическая `get_resource_layers()` заменяется методом экземпляра `layers(protocol)`; три вызова в редакторе получают `resource_system&` инъекцией (CLAUDE.md: без глобальных аксессоров).
- **Read-only.** `vfs_resolver` получает флаг `writable` (по умолчанию `true`): при `false` `store()` возвращает `false` с предупреждением. Нужен для `engine://`.
- **Запись тега, который есть только в базе движка.** `store("res://shaders/x.frag")` создаёт копию в проекте, она перекрывает движковую (shadow). Пишем `egLOG_WARN` с обоими путями; UI-подсветку не делаем (вопрос 13).
- **`safe_path` (после #377)** применяется к каждому корню каждого протокола без изменений. Корни `cache://`, `user://`, `memory://` создаются (`create_directories`) при монтировании: `weakly_canonical` несуществующего корня дал бы путь вне проекта при симлинках выше по дереву. Тест: тег с `..` отвергается для каждого протокола.
- **Watcher** уже следит за всеми корнями `res://` (§1.1), остаётся покрыть тестом изменения в обоих корнях и подавление shadow. Для `cache://` и `user://` watcher не нужен: на них нет hot reload. Конструктор `vfs_resolver` получает флаг `watch`.
- **Движковый код остаётся на `res://`** (шейдеры можно перекрыть из проекта, как сказано в ADR-02). `engine://` — для явного обращения к базе в обход проекта (вопрос 11).
- **Стык с EPIC-36.** В `EPIC-36.md:58` путь `edt://` вычисляется как `cfg_res_path/../data`. С проектом `resource/path` указывает в проект, и `edt://` уехал бы туда. Нужно считать `editor/data_path` от конфига установки редактора (`editor.cfg`), а не от `resource/path`. Это надо согласовать с сессией EPIC-36 до #161 (вопрос 9).

### 2.4. `user://` и `cache://`

- `cache://` = `<project>/.cache`. Потребители позже: asset database (ADR-05), миниатюры (ADR-11), автосохранение (EPIC-60). В M1 — только протокол и `memory://` внутри.
- `user://` по ADR-02 — данные игрока, в Play — тестовый профиль. Play Mode (ADR-07, #39) ещё нет. Предлагается: в редакторе `user://` всегда указывает на `<project>/.cache/user_profile`, а ОС-каталог (`%APPDATA%/Snake/<name>/` на Windows, `$XDG_DATA_HOME/snake/<name>/` на Linux) использует только player (#23). Так редактор никогда не трогает настоящие сохранения (вопрос 8).
- Каталог ОС даёт новая функция `platform::user_data_dir()` (`SHGetKnownFolderPath(FOLDERID_RoamingAppData)` / XDG). Вызовы: `user://` в player, настройки редактора (вопрос 9), Recent Projects.

### 2.5. CLI и конфиг

- `--project <path>`: файл `.snkproj` или каталог с `project.snkproj`. Объявляется в `cfg::initialize_configs` и пишет cfg-переменную `project/file` (то есть равно `--set project/file <path>`, путь от рабочего каталога).
- Порядок применения: `--config` (конфиг установки редактора: база движка, `edt`, логгер) → манифест проекта (корни, стартовая сцена) → `--set` (последним, перекрывает всё). При открытом проекте `resource/path` из `editor.cfg` игнорируется с предупреждением (вопрос 3).
- Ошибки открытия (нет файла, битый JSON, неверная схема) → `egLOG_ERROR` с путём и JSON-путём поля, код возврата `2`, окно не создаётся (AC #153 «понятная ошибка»).
- `editor/project_dir` (MCP) по умолчанию = корень проекта; явное значение по-прежнему главнее.
- Заголовок окна: `Snake Editor — <name>` (`editor_module.cpp:46`).

### 2.6. Версия движка

- Источник один: `SNK_SDK_VERSION` из CMake → сгенерированный `snk/common/snk_version.h` (`configure_file`) с `SNK_VERSION_MAJOR/MINOR/PATCH` и `SNK_API prj::engine_version current_engine_version()`.
- Сравнение — чистая функция `prj::compare(project, editor) → version_match { exact, patch_differs, project_older, project_newer }`, покрытая таблицей тестов. До 1.0 минорная версия считается мажорной (semver).
- Поведение (рекомендация, вопрос 5):
  - `exact` или `patch_differs` → открываем; во втором случае `egLOG_INFO`.
  - `project_older` → диалог «Проект для движка X, редактор Y: [Открыть без миграции] [Выход]».
  - `project_newer` → диалог «Нужен редактор ≥ X: [Выход]».
  - Без UI (headless, CI) — код возврата `3`, если не передан `--allow-engine-version-mismatch`.
- Как показать диалог до монтирования. Версия проверяется в `main` сразу после разбора манифеста. При расхождении редактор стартует без проекта (режим из вопроса 1) и сразу открывает диалог через `dialog_service`. «Открыть» перезапускает процесс с `--project … --allow-engine-version-mismatch`; перезапуск — модель переключения проектов для M1 (#158). Так нет полусмонтированного состояния.

### 2.7. Игровые модули (#159) — только стык

- `modules` разбирается в #153. В #159 — сопоставление имён со статическим реестром модулей source-сборки (`SNK_DECLARE_STATIC_MODULE`): игра собирается в редактор через `-DSNK_PROJECT_DIR=<project>` и `add_subdirectory(<project>/code)`. Не найден модуль, не отмеченный `optional`, → `egLOG_WARN` и строка в problems; редактор работает дальше.
- Загрузка DLL, `module_source`, `registration_context` — #21, #218, #220 (DISTRIBUTION §2). Здесь не дублируется.
- Шаблон проекта, `find_package(snake_sdk)`, `DebugGame` — #160 (теперь в #217). Шаблон кладёт `project.snkproj` этого формата.

---

## 3. Разбиение на PR

Порядок: PR-1 → PR-2 → {PR-3, PR-4, PR-7} параллельно → PR-5 → PR-6.
Внешние предусловия: #377 (`safe_path`) влит до PR-2; EPIC-36 #161/#162 (`edt://`, иконки и шаблоны в `Editor/data`) влиты до PR-5.

| PR | Истории | Что | Размер | Зависит от |
|---|---|---|---|---|
| PR-1 | #153 (ч.1), #156 (ч.1) | Модуль `snk::project`: `parse_manifest`/`load_manifest`, `snk_version.h`, `prj::compare`. Поведение редактора не меняется | S–M | — |
| PR-2 | #153 (ч.2), #154 | `res::mount_table`, `resource_system(tasks, mounts)`, `layers()` экземпляра; `res://` проект → движок, `engine://` read-only; `--project`, `project_context`, заголовок окна, `start_scene` | M | PR-1, #377 |
| PR-3 | #155 | `cache://`, `user://` (тестовый профиль), `memory://` → `.cache/memory`, `platform::user_data_dir()`, `build_dir` в контексте, MCP `project_dir` по умолчанию | M | PR-2 |
| PR-4 | #156 (ч.2) | Проверка версии в `main`, диалог, `--allow-engine-version-mismatch`, коды возврата | S | PR-2 |
| PR-5 | #157 | Тестовый проект; игровое и тестовое содержимое `Editor/res` переезжает в него; тесты, гейт desc, smoke, `release.yml` переходят на `--project` | M–L (в основном переносы) | PR-2, PR-3, EPIC-36 #162/#163 |
| PR-6 | #158 | File → New Project (из шаблона-каталога) / Open Project / Recent (до 5), переключение перезапуском | M | PR-3, PR-4, вопрос 9 (стык с #164) |
| PR-7 | #159 | `modules` → статический реестр, предупреждение при отсутствии | S–M | PR-2 (DLL-часть — #21/#218) |

### Тесты (что проверяет CI)

- **PR-1.** `tests/prj_manifest_tests.cpp`: валидный манифест; каждое обязательное поле отсутствует или неверного типа → ошибка с JSON-путём; неизвестное поле → предупреждение; `format_version` из будущего → ошибка; `content_roots` с `..`/абсолютным путём → ошибка. `tests/prj_version_tests.cpp`: таблица `compare` (exact/patch/older/newer, правило 0.x).
- **PR-2.** `tests/res_vfs_resolver_tests.cpp` + новый `res_mount_tests.cpp`: файл проекта перекрывает движковый; fallback на движок; `store` пишет в проект; `store` тега, который есть только в движке, создаёт shadow и предупреждение; `engine://` `store` → `false`; `exists` по слоям; watcher уведомляет об изменении в обоих корнях и глушит shadow; `..` отвергается в каждом протоколе. Интеграционный тест: временный каталог с манифестом → `project_context` → `resource_system` → `require_sync` ресурса проекта и ресурса движка.
- **PR-3.** Корни создаются; `user://` в редакторе указывает в `.cache/user_profile`; `memory://` в `.cache/memory`; `edt_import_cancel_tests` (пишет в `memory://`) зелёный.
- **PR-4.** Решение «открыть / диалог / код возврата» для каждого `version_match`, без окна.
- **PR-5.** Все тесты на `SNK_TEST_PROJECT_DIR` вместо `SNK_EDITOR_RES_DIR`. Гейт `desc_resave --check <project>/res --base core/res`. Smoke: `run_headless.sh --project <project>` и ещё один прогон с копией проекта в `$RUNNER_TEMP` — доказательство «вне репо». Эталонные PNG **не меняются**: ресурсы те же байты.
- **PR-6.** Логика Recent: дедуп, порог 5, порядок, битый файл → пустой список. Новый headless-сценарий `.actions` для меню — по желанию, без нового эталона.
- **PR-7.** Найден/не найден/`optional` модуль; редактор не падает без модуля.

### Риски

- **Visual smoke.** PR-1…PR-4 и PR-7 сохраняют режим без проекта, smoke в них идёт по старому пути — риск низкий. PR-5 — единственный, где меняются входы smoke. Требование: эталоны не трогаются; если кадр поменяется, это баг переноса, а не повод обновить PNG.
- **Упаковка.** `release.yml` меняется только в PR-5: в zip кладётся тестовый проект `projects/sandbox/`, ярлык запуска `bin\editor.exe --config config\editor.cfg --project projects\sandbox`. `res_core/` остаётся. Портативный zip #223 и установщик #225 подхватят эту раскладку.
- **SDK.** Ресурсы в SDK не входят (§1.4), `package_consumer` не задет. Новые свободные функции в публичных заголовках (`prj::*`, `platform::user_data_dir`) помечаются `SNK_API`, иначе упадёт shared-линковка в полном tier'е.
- **Смена сигнатуры `resource_system`.** Его конструируют тесты (`tests/null_engine_fixture.hpp`, `res_system_*_tests.cpp`). Добавляем перегрузку, а не меняем существующую, — старые тесты не трогаем.
- **EPIC-36 параллельно.** Конфликт по `editor_module.cpp`, `editor.cfg`, `Editor/res`. PR-5 ждёт #162/#163; PR-2 трогает `editor_module.cpp` только в строке заголовка.
- **#377.** Пока не влит, `safe_path` гоняется на `store`/`erase`; PR-2 его не трогает и ждёт слияния.

### Вне объёма

Загрузка DLL и hot reload (#21, #218, #220, #222); шаблон CMake и сборка игры (#160, #221 в #217); Play Mode и изоляция `user://` во время Play (#39); asset database и GUID в `cache://` (EPIC-57); переключение проекта без перезапуска; браузер проектов / лаунчер (#226); миграция проекта между версиями (ADR-06).

---

## 4. Вопросы владельцу

Каждый вопрос — с вариантами и рекомендацией. Решённое в ADR-02 здесь не переспрашивается: формат JSON, имена протоколов, `.cache` и `build` внутри проекта.

1. **Запуск без проекта.** (A) Как сейчас: корни из `editor.cfg` (`resource/path`), предупреждение «проект не открыт». (B) Пустой стартовый экран с New/Open/Recent. (C) Автоматически открыть встроенный тестовый проект. **Рекомендация:** A на время M1 (не ломает CI, smoke и zip), после PR-6 — B; режим A убрать отдельной задачей после M1.
2. **Где тестовый проект.** (1) В этом репозитории, но вне каталогов движка: `projects/sandbox/` (CI просто его находит; независимость доказывает прогон с копией в `$RUNNER_TEMP`). (2) Отдельный репозиторий как submodule. (3) Отдельный репозиторий, клонируемый в CI. **Рекомендация:** (1). Вариант (2) усложняет каждую PR, которая меняет и движок, и уровни.
3. **`--config` при открытом проекте.** (a) `--config` остаётся конфигом установки редактора (база движка, `edt`, логгер); `resource/path` из него игнорируется с предупреждением; `--set` главнее всего. (b) `--config` и `--project` взаимоисключающие. (c) Проект приносит свой `config/project.cfg`, который грузится после `editor.cfg`. **Рекомендация:** (a); (c) — позже, если появится потребность.
4. **Как редактор находит базу движка без `--config`.** (a) `--config` обязателен, как сейчас. (b) Конфиг по умолчанию рядом с exe: `<exe>/../config/editor.cfg` в zip; в дереве сборки CMake кладёт его в `build/<preset>/config/` с относительными путями до исходников. **Рекомендация:** (b), с (a) как явным перекрытием. Нужно для «двойного клика по `.snkproj`» и для #158.
5. **Политика версий.** (i) Только точное совпадение. (ii) Совпадение `MAJOR.MINOR` (до 1.0 — минор как мажор), patch свободен; старее → диалог «открыть без миграции»; новее → отказ. (iii) Диапазон в манифесте (`">=0.2 <0.3"`). **Рекомендация:** (ii); диапазоны — когда появятся реальные несовместимости.
6. **Строгость JSON.** (a) Строгий JSON; неизвестные поля — предупреждение. (b) Разрешить комментарии (`boost::json::parse_options::allow_comments`). **Рекомендация:** (a): редактор будет переписывать манифест (New Project, позже миграции) и при записи потеряет комментарии.
7. **Несколько `content_roots`.** (a) Список, первый — самый приоритетный и принимает запись. (b) Ровно один корень, поле — строка. **Рекомендация:** (a), в шаблоне и тестовом проекте — один корень `res`.
8. **`user://` в редакторе вне Play.** (a) Всегда тестовый профиль `<project>/.cache/user_profile`; ОС-каталог только у player. (b) ОС-каталог и в редакторе, тестовый профиль только в Play. **Рекомендация:** (a): редактор не может испортить настоящие сохранения, а Play (#39) позже ничего не меняет.
9. **Где хранить Recent Projects и настройки редактора (стык с #164).** EPIC-36 кладёт `edt://settings/` в `Editor/data/settings`, то есть в каталог установки: после установщика (#225) он доступен только на чтение, и настройки общие для всех пользователей. Варианты: (a) `edt://settings/` указывает на ОС-каталог пользователя `%APPDATA%/Snake/Editor/` (у `edt://` два корня: `Editor/data` только на чтение + пользовательский сверху на запись). (b) Отдельный протокол для настроек редактора. (c) `user://` — нет, это данные игрока проекта (ADR-02). **Рекомендация:** (a). Это меняет план #164: нужно согласие владельца и сессии EPIC-36. Там же: `editor/data_path` должен считаться от `editor.cfg`, а не от `resource/path` (§2.3).
10. **`.snk/` и `.cache/`.** (a) Оставить оба: `.snk/` — состояние сессии и политика (`mcp.json`, `agent_policy.json`, правит человек), `.cache/` — регенерируемое. (b) Перенести `mcp.json` в `.cache/`. **Рекомендация:** (a): `agent_policy.json` коммитится, кэш — нет.
11. **Движковый код: `res://` или `engine://`.** (a) Шейдеры и базовые desc движок запрашивает через `res://`, проект может их перекрыть. (b) Через `engine://`, проект перекрыть не может. **Рекомендация:** (a), как в ADR-02; `engine://` — для явного обращения к базе в обход проекта (например, «вернуть движковое значение»).
12. **Принадлежность #159.** Её объём совпадает с EPIC-52 #21, у которого нет историй. (a) Перевесить #159 в #21, как #160 в #217. (b) Оставить в #19 только «манифест → список модулей + предупреждение», загрузку целиком отдать #21. **Рекомендация:** (b): PR-7 закрывает #159 в этом объёме, а #21 получает историю «загрузка модулей из `project_context`».
13. **Запись в ресурс, который есть только в движке.** (a) Создать копию в проекте (shadow) и предупредить в логе. (b) Отказать, пока пользователь явно не выберет «Override in project». **Рекомендация:** (a) в M1; (b) — вместе с Asset Browser по слоям, отдельной задачей.
14. **Имя каталога `user://` в ОС.** (a) `Snake/<name>` из манифеста. (b) `Snake/<name>-<короткий хеш пути проекта>` — чтобы два проекта с одним именем не делили сохранения. **Рекомендация:** (a) для player (имя игры — это идентичность сохранений); в редакторе вопрос снимает вопрос 8 (a).
