# EPIC-05: Entity Management — продвинутые операции над сущностями

**Theme:** editor
**Status:** in_progress
**Depends on:** EPIC-08, EPIC-09 (для desc-driven варианта)

## Цель

Полноценные операции над объектами сцены: переименование, дублирование,
изменение родителя (re-parent), поиск в иерархии.

> **Примечание:** реализовывать имеет смысл ПОСЛЕ EPIC-08/EPIC-09,
> так как все операции должны идти через `world_desc` мутацию, а не через ECS.

---

## US-05-1: Rename entity

**Файлы:** `edt_scene_hierarchy_panel.cpp`

Двойной клик на узле в иерархии → inline InputText.
После подтверждения (Enter / потеря фокуса):
- `node->name` обновляется в `m_world_desc`
- `serialize_and_push` → hot-reload

Валидация: имя не пустое, нет дубликата в siblings.

> Сейчас inline rename есть, но пишет только в `name_component` ECS —
> не сохраняется в desc.

---

## US-05-2: Duplicate entity

**Файлы:** `edt_scene_hierarchy_panel.cpp`, `edt_editor_system.cpp`

Context menu → "Duplicate".
Глубокая копия `prefab_node` (включая children и components).
Новое имя = `<original_name>_copy` (или `_copy_2` если занято).
Вставка как sibling. `serialize_and_push`.

---

## US-05-3: Re-parent (drag в иерархии)

**Файлы:** `edt_scene_hierarchy_panel.cpp`, `edt_editor_system.cpp`

Drag-and-drop узлов внутри иерархии:
- Перетащить узел на другой узел → сделать дочерним
- Перетащить между узлами → изменить порядок
- Учесть трансформацию: при смене родителя пересчитать
  `position/rotation/scale` относительно нового родителя

Реализация:
```
ImGui::BeginDragDropSource() / ImGui::BeginDragDropTarget()
Payload = имя перетаскиваемого узла
```

---

## US-05-4: Search / Filter в иерархии

**Файлы:** `edt_scene_hierarchy_panel.cpp`

InputText в шапке панели. Фильтрация по имени узла (case-insensitive substring).
Совпадающие узлы подсвечиваются, родители раскрываются автоматически.

> Частично реализовано (filter bar есть), но работает через ECS итерацию.
> После US-26 (EPIC-10) — адаптировать под `prefab_node` дерево.

---

## US-05-5: Multi-select

**Файлы:** `edt_scene_hierarchy_panel.cpp`, `edt_editor_system.cpp`

Ctrl+Click — добавить к выделению.
Shift+Click — выделить диапазон.
Действия над группой: Delete, Duplicate, Group (создать пустой родитель и переместить).

---

## Критерии готовности

- [x] Rename обновляет `world_desc` через hot-reload (`on_rename_node` callback → `serialize_and_push`)
- [x] Duplicate создаёт полную копию узла в `world_desc` (`duplicate_entity`, вставка как sibling)
- [ ] Drag-and-drop re-parent работает с пересчётом transform
- [x] Filter работает с `prefab_node` деревом (рекурсивный `node_matches_filter`, auto-expand родителей)
- [ ] Multi-select: delete и duplicate для группы
