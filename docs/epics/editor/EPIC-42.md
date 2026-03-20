# EPIC-42: Workspace & Game Project System — разделение движка и игровых данных

**Status:** planned
**Theme:** editor
**Dependencies:** EPIC-36 (Editor VFS Protocol — `edt://`)

---

## Мотивация

Сейчас игровые ресурсы (`levels/`, `objects/`, `shaders/`) и код редактора живут в одном репозитории, в одной папке `Editor/res/`. Это создаёт проблемы:

1. **Нет разделения движок/игра.** Ресурсы snake-игры намертво вшиты в репозиторий движка. Чтобы начать новую игру — нужно форкать весь движок или вручную чистить `res/`.

2. **Нет концепции "проекта".** Редактор не знает, с каким набором данных он работает. Нет workspace path, нет project settings, нет переключения между проектами.

3. **Невозможна модульность.** Хочется: подключил другой git submodule — работаешь с другой игрой. Движок как инструмент, игра как данные.

## Архитектурное решение

### Целевая структура

```
OpenGL_Engine/                         ← движок
├── Engine/
│   └── res/                           ← движковые defaults (шейдеры, base materials)
│         ├── shaders/
│         └── defaults/
├── Editor/
│   └── data/                          ← edt:// — ресурсы редактора (EPIC-36)
│
└── games/
      └── snake_game/                  ← git submodule — конкретная игра
            ├── workspace.json         ← манифест проекта
            ├── res/                   ← игровые ресурсы (overlay поверх Engine/res/)
            │   ├── levels/
            │   ├── objects/
            │   ├── skybox/
            │   └── shaders/           ← game-specific шейдеры (override движковых)
            ├── config/
            │   └── game.cfg
            └── code/                  ← C++ игровая логика (→ DLL)
                └── CMakeLists.txt
```

### Layered VFS — один протокол `res://`, два mount point'а

`vfs_resolver` уже поддерживает стек `entries_stack` с приоритетом: `resolve_tag()` перебирает mount points сверху вниз, первый найденный файл побеждает. Нужно только добавить второй mount point.

```cpp
// Текущее (один mount):
auto& resolver = registrate_resolver<vfs_resolver>("res", {res_path});

// Целевое (layered — game overlay поверх engine defaults):
auto& resolver = registrate_resolver<vfs_resolver>("res", {
    game_res_path,    // ← приоритет: ищем сначала тут
    engine_res_path   // ← fallback: движковые defaults
});
```

Семантика:
- `res://levels/main.desc` → ищет в `games/snake_game/res/levels/main.desc`, потом `Engine/res/levels/main.desc`
- `res://shaders/pbr.glsl` → игра может override движковый шейдер, положив свой в `games/snake_game/res/shaders/pbr.glsl`
- `store()` пишет в top mount (game folder) — `entries_stack.top()`

Протоколов остаётся два: `res://` (layered) и `edt://` (EPIC-36).

### Game code — DLL runtime loading

Заготовка уже есть: `game_module_service_interface` с `load_game_runtime()` / `unload_game_runtime()`. Сейчас `GameModuleService` создаёт `GameModule` статически. Целевой подход:

```
Editor (нажатие Play)
  → GameModuleService::load_game_runtime()
    → LoadLibrary("games/snake_game/build/game.dll")
    → GetProcAddress("create_game_module")
    → module_interface* = create_game_module()
    → module->register_services(data)
    → module->initialize_services(data)

Editor (нажатие Stop)
  → GameModuleService::unload_game_runtime()
    → module->shutdown_services(data)
    → FreeLibrary(dll_handle)
```

**Два режима:**
- **Editor**: DLL loading → hot reload через unload/recompile/reload → restart уровня
- **Production build**: `add_subdirectory(game/code)` → статическая линковка → один `.exe`

### Открытые вопросы (требуют брейншторма)

1. **Play mode UI** — отдельное окно? Вкладка в редакторе? ImGui viewport? Нужен ли separation между editor world и game world?
2. **Game DLL API surface** — что именно экспортирует DLL? Только `create_game_module()`? Или ещё `get_game_info()` для метаданных?
3. **Шейдеры — engine или game?** Текущие шейдеры (pbr, z_prepass, composition) — часть рендер-пайплайна. Скорее engine. Но game может хотеть custom post-effects. Решить при миграции.
4. **base_*.desc** — нужны ли ещё? Изначально для наследования, но возможно уже не используются. Проверить и решить.
5. **Hot reload granularity** — первая версия: перезагрузка DLL = restart уровня. Позже можно сериализацию ECS состояния.

## User Stories

### US-42-1: Workspace config и CLI-аргумент
**Файлы:** `Editor/code/editor_system/core/editor_module.cpp`, `Editor/config/editor.cfg`

Добавить настройку `editor.workspace_path` и CLI `--workspace`. При старте редактор знает путь к текущему game project.

**AC:**
- [ ] `CFG_VAR_DEF_STR` для `editor.workspace_path` с default `"games/snake_game"`
- [ ] CLI `--set editor.workspace_path=games/rpg` работает
- [ ] При отсутствии workspace path — warning в лог, редактор запускается с одним engine mount

### US-42-2: Layered VFS — два mount point'а для `res://`
**Файлы:** `core/core/resource/res_system.cpp`, `Editor/code/editor_system/core/editor_module.cpp`
**Зависимости:** US-42-1

Передать два пути в `vfs_resolver`: game res (приоритетный) и engine res (fallback). Механизм уже реализован через `entries_stack`.

**AC:**
- [ ] `res://` resolver принимает два mount point'а: `{workspace}/res/` и `Engine/res/`
- [ ] Файл из game folder находится первым при `resolve_tag()`
- [ ] Файл из engine folder находится если в game его нет
- [ ] `store()` пишет в game folder (top of stack)
- [ ] File watcher работает на оба mount point'а

### US-42-3: Миграция engine defaults в `Engine/res/`
**Файлы:** `Editor/res/` → `Engine/res/` (частично) + `games/snake_game/res/` (частично)

Разделить текущие `Editor/res/` на движковые defaults и игровые ресурсы.

**AC:**
- [ ] `Engine/res/` содержит шейдеры и base defaults движка
- [ ] `games/snake_game/res/` содержит: `levels/`, `objects/`, `skybox/` и game-specific ресурсы
- [ ] `Editor/res/` пуста или удалена
- [ ] Все `.desc` файлы продолжают использовать `res://` — менять пути не нужно
- [ ] Редактор запускается, уровень загружается, рендер работает

### US-42-4: Workspace manifest — `workspace.json`
**Файлы:** `games/snake_game/workspace.json`
**Зависимости:** US-42-1

Создать формат `workspace.json` и парсинг. Минимальные поля: `name`, `res_path`.

**AC:**
- [ ] `workspace.json` парсится при инициализации workspace
- [ ] Имя workspace отображается в title bar редактора (`Snake Engine — Snake Game`)
- [ ] `res_path` из manifest используется для layered VFS mount
- [ ] Отсутствие `workspace.json` — fallback на defaults (`res_path = "res/"`)

### US-42-5: Game submodule setup и документация
**Файлы:** `.gitmodules`, `games/snake_game/`, `CLAUDE.md`, `README.md`
**Зависимости:** US-42-3

Оформить `games/snake_game` как git submodule. Документировать workflow.

**AC:**
- [ ] `games/snake_game` — git submodule
- [ ] `CLAUDE.md` содержит секцию "Workspace & Game Projects"
- [ ] `README.md` обновлён
- [ ] Задокументирован workflow: создание новой игры, переключение между играми

### US-42-6: UI — Open Workspace / Recent Workspaces
**Файлы:** `Editor/code/editor_system/controller/edt_editor_layer.cpp`
**Зависимости:** US-42-4

Пункт меню File → Open Workspace... и список recent workspaces.

**AC:**
- [ ] File → Open Workspace... открывает folder picker
- [ ] При выборе папки — проверка наличия `workspace.json` или `res/`
- [ ] Recent Workspaces подменю (до 5 записей)
- [ ] Title bar обновляется при переключении
- [ ] Первая версия: переключение workspace = restart редактора (hot switch позже)

### US-42-7: DLL Game Module — загрузка игровой логики в runtime
**Файлы:** `core/engine/game_system/game_module_service.h`, `core/core/application/app_game_module_service_interface.h`
**Зависимости:** US-42-1

Переписать `GameModuleService` для загрузки game module из DLL вместо статического создания.

**AC:**
- [ ] `LoadLibrary()` / `FreeLibrary()` для загрузки game DLL
- [ ] DLL экспортирует `create_game_module()` → возвращает `module_interface*`
- [ ] `load_game_runtime()` загружает DLL и вызывает lifecycle (register → initialize)
- [ ] `unload_game_runtime()` вызывает shutdown → `FreeLibrary()`
- [ ] При отсутствии DLL — warning, редактор работает без game логики

### US-42-8: Play/Stop mode в редакторе
**Файлы:** `Editor/code/editor_system/`
**Зависимости:** US-42-7

Кнопки Play/Stop в toolbar. Play загружает game DLL, Stop выгружает.

**AC:**
- [ ] Toolbar: кнопки Play / Stop
- [ ] Play → `load_game_runtime()` → game systems начинают update
- [ ] Stop → `unload_game_runtime()` → возврат к editor mode
- [ ] Состояние сцены сохраняется перед Play и восстанавливается после Stop

### US-42-9: CMake — game DLL target и production static build
**Файлы:** `CMakeLists.txt`, шаблон `games/snake_game/code/CMakeLists.txt`
**Зависимости:** US-42-7

CMake конфигурация для компиляции game code как DLL (editor) или static lib (production).

**AC:**
- [ ] `games/snake_game/code/CMakeLists.txt` — шаблон game project'а
- [ ] `cmake -DGAME_BUILD_TYPE=DLL` → `.dll` для editor hot-loading
- [ ] `cmake -DGAME_BUILD_TYPE=STATIC` → статическая линковка для production
- [ ] Game target линкуется к `snk::engine` (доступ к ECS, ресурсам, сценам)

---

## Порядок выполнения

```
EPIC-36 (edt://)
  └── US-42-1 (workspace config)
        ├── US-42-2 (layered VFS)
        │     └── US-42-3 (миграция ресурсов)
        │           └── US-42-5 (submodule + docs)
        ├── US-42-4 (workspace.json)
        │     └── US-42-6 (UI open/recent)
        └── US-42-7 (DLL game module)
              ├── US-42-8 (play/stop mode)
              └── US-42-9 (CMake DLL/static)
```

**Фаза 1** (ресурсы): US-42-1 → US-42-2 → US-42-3 → US-42-4 → US-42-5 → US-42-6
**Фаза 2** (game code): US-42-7 → US-42-8 → US-42-9

Фазы можно делать параллельно.

## Риски

- **Layered resolve + store** — `store()` пишет в top mount (game). Если ресурс существует только в engine mount и пользователь его "сохраняет" — создастся копия в game folder (shadow). Это может быть и фича, и проблема. Митигация: предупреждение в UI при shadow-write.
- **DLL ABI stability** — game DLL и editor должны быть собраны одним компилятором с одинаковыми флагами, иначе UB. Митигация: single CMake project, DLL собирается из того же build tree.
- **Play mode isolation** — при Play game systems имеют доступ ко всему `app_data_storage`. Баг в game коде может сломать editor state. Митигация: scoped child storage для game module (будущее).
- **Шейдеры** — решить при US-42-3: какие шейдеры engine, какие game. Текущие (pbr, z_prepass, composition) скорее engine.
- **base_*.desc** — проверить используются ли. Если нет — удалить при US-42-3.
- **Hot reload** — первая версия: полная перезагрузка DLL + restart уровня. Сериализация ECS состояния — отдельный эпик.

## Критерии завершения эпика

- [ ] `res://` layered VFS: game overlay поверх engine defaults
- [ ] Игровые ресурсы физически отделены от движка в `games/snake_game/`
- [ ] Workspace переключается через config, CLI или UI
- [ ] Title bar: `Snake Engine — <Workspace Name>`
- [ ] Game DLL загружается/выгружается через Play/Stop
- [ ] Production build: статическая линковка game code
- [ ] Редактор работает без регрессий
- [ ] Документация описывает workflow создания новой игры
