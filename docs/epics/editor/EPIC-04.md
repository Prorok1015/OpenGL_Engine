# EPIC-04: Scene Save/Load — сохранение и управление сценами

**Theme:** editor
**Status:** done

## Реализовано

### Загрузка уровня
- File → Open Level... — file dialog с фильтром `.desc`
- `level_manager->load(tag)` — full desc pipeline → ECS через assembler
- Чтение world names из live level, populate `m_world_names`
- Чтение systems per-world из `level_desc` для последующего сохранения

### Сохранение уровня
- File → Save Level — modal с path input (relative to res://)
- File → Save Level (Ctrl+S) — quick save если тег известен
- `edt::serialize_level()` (`edt_scene_serializer.hpp`) — ECS → JSON
  - Обходит `children_component` дерево
  - `component_tracker` → tracked components с overrides
  - Camera: позиция из `mouse_controller_component`
  - Prefab instances: сериализуются как `prefab_desc` ссылки
- Запись в файл через `std::ofstream`

### Новый уровень
- `new_level()` — строит level/world_desc JSON → store → level_manager->load
- Дефолтный мир: camera_desc, directional_light_desc, anchor

### Dirty tracking + Exit confirmation
- `m_is_dirty` — флаг несохранённых изменений
- `show_exit_confirm()` — modal "Save & Continue / Discard / Cancel"
- Вызывается при File → Exit и File → New Level если dirty

### Мульти-мир (US-18)
- World tabs в `scene_hierarchy_panel`
- `switch_to_world(idx)` — переключает активный registry
- `create_world(name)` — создаёт новый мир через desc pipeline

## Ключевые файлы

- `Editor/code/editor_system/edt_editor_system.cpp` — load/save/new_level/create_world
- `Editor/code/editor_system/edt_scene_serializer.hpp` — ECS→JSON (deprecated при EPIC-09)
- `Editor/code/editor_system/panels/edt_dockspace.h/.cpp` — меню File

## Устаревает при

EPIC-09 (US-25: simplified save — desc→disk без `edt_scene_serializer`).
