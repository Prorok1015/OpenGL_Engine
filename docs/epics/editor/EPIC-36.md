# EPIC-36: Editor VFS Protocol — `edt://` для ресурсов редактора

**Status:** planned
**Theme:** editor
**Dependencies:** none

---

## Мотивация

Сейчас все ресурсы — и игровые (модели, шейдеры, уровни), и редакторные (иконки, шаблоны десков, лого) — лежат в одной папке `Editor/res/` и доступны через единый протокол `res://`. Это создаёт несколько проблем:

1. **Нет границы между editor и game ресурсами.** Иконки кнопок (`icons/`), шаблоны (`templates/`) и тестовые файлы (`test.jpg`, `block.png`) лежат рядом с игровыми ресурсами (`objects/`, `levels/`, `shaders/`). При будущем экспорте проекта нужно вручную исключать editor-файлы.

2. **Asset Browser показывает служебные файлы.** `icons/`, `templates/`, тестовые `.desc` — всё видно пользователю, хотя он не должен их трогать.

3. **Нет места для пользовательских настроек.** Layout окон (ImGui `.ini`), список recent files, настройки профиля — некуда положить в текущей структуре. Они не игровые ресурсы и не должны быть в `res://`.

4. **Шаблоны жёстко привязаны к `res://`.** `res::tag::make("templates/level.desc")` — если потом `templates/` нужно перенести, придётся менять все вызовы.

Решение: зарегистрировать новый VFS-протокол `edt://`, резолвящий в отдельную директорию с ресурсами редактора. Инфраструктура уже есть — `resource_system::registrate_resolver<vfs_resolver>("edt", path)`.

## Архитектурное решение

### Структура директорий

```
Editor/
├── res/                              ← res:// — только игровые ресурсы
│   ├── shaders/
│   ├── objects/
│   ├── levels/
│   ├── skybox/
│   ├── base_material.desc
│   ├── base_texture.desc
│   └── base_geometry.desc
│
└── data/                             ← edt:// — ресурсы редактора
    ├── icons/
    │   ├── editor_engine_logo.png
    │   ├── engine_logo.png
    │   └── engine_logo_small.png
    ├── templates/
    │   ├── level.desc
    │   └── world.desc
    ├── defaults/                     ← fallback текстуры/материалы (будущее)
    └── settings/                     ← .gitignore'd, пользовательские настройки
        ├── layout.ini                ← ImGui dockspace layout
        ├── recent_files.json         ← список последних файлов
        └── user_prefs.cfg            ← тема, шрифт, горячие клавиши
```

### Регистрация протокола

```cpp
// editor_module.cpp::initialize_services()
auto edt_data_path = std::filesystem::path(cfg_res_path.value()) / ".." / "data";
res.registrate_resolver<res::vfs_resolver>("edt", edt_data_path);
```

### Конфигурация

```yaml
# editor.cfg
editor:
    data_path: ../data/    # путь к Editor/data/ относительно build/
```

## User Stories

### US-36-1: Регистрация `edt://` резолвера
**Файлы:** `Editor/code/editor_system/core/editor_module.cpp`, `Editor/config/editor.cfg`

Зарегистрировать `edt://` протокол в `editor_module::initialize_services()`, указав путь к `Editor/data/`.

**AC:**
- [ ] В `editor.cfg` добавлена настройка `editor.data_path` (default: `../data/`)
- [ ] `editor_module` регистрирует `vfs_resolver` для протокола `"edt"`
- [ ] `res::tag("edt://icons/editor_engine_logo.png")` корректно резолвит файл
- [ ] При отсутствии `Editor/data/` — warning в лог, не crash (graceful fallback)

### US-36-2: Перенести icons и templates в `Editor/data/`
**Файлы:** `Editor/res/icons/` → `Editor/data/icons/`, `Editor/res/templates/` → `Editor/data/templates/`, `Editor/code/editor_system/core/editor_module.cpp`, `Editor/code/editor_system/controller/edt_editor_system.cpp`, `Editor/code/editor_system/controller/edt_level_controller.cpp`, `Editor/code/editor_system/controller/edt_world_controller.cpp`
**Зависимости:** US-36-1

Физически переместить папки и обновить все `res::tag::make("icons/...")` → `res::tag("edt://icons/...")`, аналогично для `templates/`.

Текущие использования:
- `editor_module.cpp:33` — `res::tag::make("icons/editor_engine_logo.png")`
- `edt_editor_system.cpp:50-51` — warmup `templates/level.desc`, `templates/world.desc`
- `edt_level_controller.cpp:255` — `res::tag::make("templates/" + filename)`
- `edt_world_controller.cpp:48` — `res::tag::make("templates/world.desc")`

**AC:**
- [ ] `Editor/data/icons/` содержит 3 иконки (перемещены из `Editor/res/icons/`)
- [ ] `Editor/data/templates/` содержит `level.desc`, `world.desc`
- [ ] Все обращения к иконкам и шаблонам используют `edt://` протокол
- [ ] `Editor/res/icons/` и `Editor/res/templates/` удалены
- [ ] Редактор запускается, иконка отображается, создание уровня/мира работает

### US-36-3: Убрать тестовые файлы из `res://`
**Файлы:** `Editor/res/`

В корне `Editor/res/` лежат файлы, не являющиеся ни игровыми, ни editor-ресурсами: `test.jpg`, `test2.png`, `block.png`, `milla.bmp`, `window.png`, `test_desc.desc`, `test_field_desc.desc` и т.д. Нужно определить их судьбу.

**AC:**
- [ ] Тестовые файлы (`test*.desc`, `test.jpg`, `test2.png`, `block.png`, `milla.bmp`, `window.png`) удалены из `Editor/res/` или перемещены в `Editor/data/test/` если используются
- [ ] Убедиться что ни один `.cpp` файл не ссылается на удалённые ресурсы
- [ ] `Editor/res/` содержит только игровые ресурсы

### US-36-4: Инфраструктура для settings — `edt://settings/`
**Файлы:** `Editor/data/settings/` (новая директория), `.gitignore`

Подготовить директорию для пользовательских настроек. Сами настройки (layout, recent files) будут реализованы в будущих эпиках — здесь только инфраструктура.

**AC:**
- [ ] `Editor/data/settings/` создана (может быть пустой с `.gitkeep`)
- [ ] `Editor/data/settings/` добавлена в `.gitignore` (кроме `.gitkeep`)
- [ ] `edt://settings/` доступен для чтения/записи через resource system
- [ ] Хелпер-функция или документация: как сохранить/прочитать файл в `edt://settings/`

### US-36-5: Asset Browser — скрыть `edt://` ресурсы
**Файлы:** `Editor/code/editor_system/panels/edt_asset_browser_panel.cpp`
**Зависимости:** US-36-2

Asset Browser показывает содержимое `res://`. После миграции editor-файлов в `edt://` они автоматически исчезнут из браузера. Но нужно убедиться, что нет регрессий.

**AC:**
- [ ] Asset Browser не показывает файлы из `Editor/data/` (они за пределами `res://`)
- [ ] Если в будущем Asset Browser получит поддержку нескольких VFS roots — `edt://` root помечен как hidden/internal
- [ ] Drag-drop из asset browser продолжает работать для `res://` ресурсов

## Порядок выполнения

```
US-36-1 (регистрация edt://)
  ├── US-36-2 (перенос icons + templates)
  │     └── US-36-5 (asset browser)
  ├── US-36-3 (убрать тестовые файлы)
  └── US-36-4 (settings инфраструктура)
```

## Риски

- **Шейдеры остаются в `res://`.** Шейдеры используются и engine-кодом (`rnd_z_prepass`, `rnd_composition_pass`), и editor-кодом. Они — часть рендер-движка, не редактора. Оставить в `res://`.
- **ImGui .ini файл.** ImGui по умолчанию пишет `imgui.ini` в рабочую директорию. Нужно перенаправить через `ImGui::GetIO().IniFilename` на `edt://settings/layout.ini` — но `edt://` это VFS-тег, а ImGui ожидает filesystem path. Решение: резолвить VFS-тег в абсолютный путь для ImGui.
- **Обратная совместимость.** Если кто-то использует `res://templates/level.desc` в `.desc` файлах — они сломаются. Проверить grep по `res://templates` и `res://icons` в `.desc` файлах.

## Критерии завершения эпика

- [ ] Протокол `edt://` зарегистрирован и резолвит в `Editor/data/`
- [ ] Иконки и шаблоны доступны только через `edt://`, убраны из `res://`
- [ ] Тестовые файлы убраны из `Editor/res/`
- [ ] `edt://settings/` готов для будущих пользовательских настроек
- [ ] Asset Browser не показывает editor-ресурсы
- [ ] Редактор запускается и работает без регрессий
