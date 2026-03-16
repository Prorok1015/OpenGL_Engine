# EPIC-03: Asset Browser — просмотр и импорт ассетов

**Theme:** editor
**Status:** done

## Реализовано

- `asset_browser_panel` — файловый браузер через `std::filesystem`
- Рекурсивный листинг директории ресурсов (`res://`)
- Иконки по типу файла (.desc, .glb, .obj, .fbx, .png, ...)
- Навигация по папкам (двойной клик на папку, кнопка "..")
- Drag-and-drop ассетов из браузера в Viewport
  - Payload `"ASSET_PATH"` — абсолютный путь файла
  - Viewport принимает payload, вызывает `on_asset_dropped` callback
- Импорт через File → Import... (file dialog, фильтр по расширению)
- После импорта: `res::get_system().warmup<scn::skinning_prototype_desc>(tag)`

## Ключевые файлы

- `Editor/code/editor_system/panels/edt_asset_browser_panel.h/.cpp`
- `Editor/code/editor_system/panels/edt_viewport_panel.cpp` (drop target)
- `Editor/code/editor_system/edt_file_dialog.h/.cpp`

## Известные ограничения (до EPIC-09)

- Drop handler знает только `skinning_prototype_desc` — не определяет тип по `__type`
- Нет превью ассетов
