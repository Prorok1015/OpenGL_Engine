# EPIC-15: Command Pattern — Undo/Redo

**Theme:** editor
**Status:** in progress
**Issue:** [#29](https://github.com/Prorok1015/OpenGL_Engine/issues/29)
**Depends on:** EPIC-08, EPIC-09 (операции через desc — легко обернуть в команды)

## Цель

Undo/Redo для всех правок сцены: история на диффах состояния (см. «Архитектура»).
Горячие клавиши Ctrl+Z / Ctrl+Y.

---

## Архитектура

**Решение владельца (2026-10-29):** история на диффах, а не на классах команд. Первоначальный план (`edt::command` с
`execute/undo` и по классу на операцию: `cmd_add_node`, `cmd_delete_node`, `cmd_transform`, `cmd_set_field`, ...)
отклонён: каждая новая правка (инспектор, гизмо, агент, A5-команды) требовала бы своего класса, и любая правка мимо
класса молча ломала бы undo. Вместо этого документ сравнивает своё состояние с последним коммитом, и всё, что прошло
через `scene_document::commit()`, попадает в историю само.

### Общая история: `document_history` (`snk/editor/edt_document_history.h`)

Не знает типа документа (сцена сейчас, data-документы #146 потом):

- `history_change` — разница одной правки: `undo()`, `redo()`, `keys()` (что изменено: узел, поле; две правки
  конфликтуют, если у них общий ключ);
- `history_record` — `id`, `label` (имя в меню Edit), `origin` (`cmd::origin`: ui / test / agent+session / game),
  список `history_change`, `group`;
- `document_history` — записи и позиция: `record(label, origin, change)` (null = ничего не изменилось, не пишется;
  отменённое (redo) отбрасывается), `undo()/redo()`, `next_undo()/next_redo()`, лимит (по умолчанию 100, старые
  уходят первыми), `clear()`.
- **Save-point**: `mark_saved()` запоминает id верхней записи, `is_at_save_point()` — вернулись ли к ней undo/redo.
  Если сохранённая запись отброшена (отменена и заменена новой правкой, ушла за лимит, откачена), сохранённого
  состояния в истории больше нет.

### Группы и `rollback_group`

- `begin_group(label, origin)` открывает группу origin'а (сессии агента), `end_group(id)` закрывает, `open_group(origin)`.
- Пока никто другой не правит, правки группы складываются в одну запись (одна строка в Undo). Правка другого origin'а
  между ними — следующая правка группы начинает новую запись той же группы после неё: undo остаётся линейным.
- `rollback_group(id)` откатывает применённые правки группы от новых к старым и удаляет их из истории. Останавливается
  перед правкой, чей ключ поменяла более поздняя запись другого origin'а (человек переименовал тот же узел): правку
  человека никогда не откатывает и не перетирает. Результат: `rolled_back`, `remaining`, `conflict_key`,
  `conflict_label`. Отменённые (redo) записи отбрасываются; если что-то откачено, save-point сбрасывается; без
  конфликта группа закрывается.

### Сцена: дифф по node id (`controller/edt_scene_history.h`)

- `scene_history_index` — на каждый мир: `node_id → {parent, previous (сосед перед ним), depth, узел без детей}`.
  Узел без детей (shallow): `name`, transform, `has_transform`, override records и по компоненту `type_name`,
  `parent_desc` (по тегу) и JSON `overrides`; `edits` (`comp_edit`) не входят — они строятся только при раскрытии.
- `commit()` обходит миры, сравнивает каждый узел с индексом и копирует только изменённые; удалённые — те, что есть
  в индексе, но не встретились. Одна запись = один `scene_change` с ключами `node:<world>:<id016x>`; узел, у которого
  поменялось только место среди неизменных соседей (сосед перед ним появился или исчез), меняется, но ключа не даёт.
- Применение (undo/redo): узлы, которые остаются, ставятся по глубине (родители раньше детей), внутри одной
  глубины — после своего предыдущего соседа; затем удаляются уходящие. Индекс обновляется вместе с мирами.
- Счётчики `last_commit_stats()`: `nodes_compared`, `snapshots_copied`, `changes`. На 2000 узлах правка одного поля —
  compared = все, copied = 1, changes = 1; удаление поддерева из k узлов — changes = k (состояния переезжают из
  индекса, не копируются).

### `scene_document`

- `is_dirty() = m_unrecorded || !history.is_at_save_point()` — undo до сохранённого состояния делает документ чистым.
- `commit(label, push)`: дифф → `history.record(label, edit_origin(), change)`; без изменений — ничего (и dirty не
  меняется). Если сравнить нельзя (узел без id, id дважды, другое число миров) — правка помечается как «не в истории»
  (`m_unrecorded`), история очищается, индекс строится заново.
- `undo()/redo()/rollback_group()` вызывают `set_before_history_apply` (указатели в миры, выделение, устаревают),
  применяют историю и пушат как коммит (`touch_descs`, pusher, `document_changed`).
- `edit_origin_scope(scene, origin)` — RAII: правки внутри записываются от имени origin'а (сессия агента).

### Границы истории

История очищается (`reset_history()`, dirty сохраняется) и начинается с текущих миров:

- загрузка уровня и New level (`populate_worlds_from_level`);
- изменение списка миров — `commit(commit_mode::state_only)` (Create world, New level) — граница, а не запись;
- перечитывание уровня с диска (`apply_world_resync`);
- уход уровня из game layer (`detach_from_game_layer`) и сборка в новом (`attach_to_game_layer`).

Перед сохранением живые трансформы пишутся в desc (`sync_ecs_transforms_to_desc`) — это `rebase_history()`: индекс
берёт миры как есть, без записи.

---

## Интеграция с UI

### Горячие клавиши

В `edt_dockspace.cpp` (`render_menu_bar`):
```cpp
if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    m_on_undo();
if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    m_on_redo();
```

### Меню Edit

```
Edit
  ├── Undo <desc>   Ctrl+Z
  └── Redo <desc>   Ctrl+Y
```

`<desc>` = `history_record::label` записи, которую применит Undo/Redo (`next_undo()/next_redo()`).

### История в Console Panel (опционально)

Вкладка "History" в `console_panel` — список последних N команд с возможностью
кликнуть и откатиться до выбранного состояния.

---

## Особые случаи

- **Transform drag** — один коммит при `mouse release` (не на каждый кадр) = одна запись
- **Batch операции** — удаление нескольких узлов = один коммит = одна запись
- **Save** — НЕ очищает историю: `mark_clean()` ставит save-point, undo после сохранения возможен
- **Load level** / **New level** / новый мир — граница истории (`reset_history()`)

---

## Критерии готовности

- [ ] `document_history` + дифф сцены: undo/redo, лимит, save-point, группы, `rollback_group` (#141–#144)
- [ ] Ctrl+Z / Ctrl+Y работают в редакторе
- [ ] Add/Delete/Duplicate/Reparent/Rename, Transform commit и правка в Inspector — каждая одна запись истории
- [ ] Меню Edit → Undo/Redo с именем записи (#145)
- [ ] Load/New level очищает историю
