# EPIC-15: Command Pattern — Undo/Redo

**Theme:** editor
**Status:** planned
**Depends on:** EPIC-08, EPIC-09 (операции через desc — легко обернуть в команды)

## Цель

Undo/Redo для всех структурных операций над сценой через Command Pattern.
Горячие клавиши Ctrl+Z / Ctrl+Y.

---

## Архитектура

Все мутации `m_world_desc` проходят через `edt::command_manager`:

```cpp
// Editor/code/editor_system/edt_command_manager.h

namespace edt {

    class command {
    public:
        virtual ~command() = default;
        virtual void execute() = 0;
        virtual void undo()    = 0;
        virtual std::string description() const = 0;
    };

    class command_manager {
    public:
        void execute(std::unique_ptr<command> cmd);  // execute + push to history
        void undo();
        void redo();
        bool can_undo() const;
        bool can_redo() const;
        void clear();

        // Для отображения в UI (история команд)
        const std::deque<std::unique_ptr<command>>& history() const;

    private:
        std::deque<std::unique_ptr<command>> m_undo_stack;  // max ~50
        std::deque<std::unique_ptr<command>> m_redo_stack;
        static constexpr size_t MAX_HISTORY = 50;
    };
}
```

---

## Команды

### `cmd_add_node` — добавить узел

```cpp
struct cmd_add_node : edt::command {
    scn::prefab_desc::prefab_node node_snapshot;  // копия добавляемого узла
    std::string parent_path;  // путь в дереве до родителя (или root)

    void execute() override { /* добавить в m_world_desc + serialize_and_push */ }
    void undo()    override { /* удалить из m_world_desc + serialize_and_push */ }
};
```

### `cmd_delete_node` — удалить узел

```cpp
struct cmd_delete_node : edt::command {
    scn::prefab_desc::prefab_node node_snapshot;  // полная копия удалённого узла
    std::string parent_path;
    size_t index;  // позиция в children для восстановления

    void execute() override { /* удалить */ }
    void undo()    override { /* вставить обратно на ту же позицию */ }
};
```

### `cmd_transform` — изменение трансформа

```cpp
struct cmd_transform : edt::command {
    std::string node_name;
    glm::vec3 old_pos, new_pos;
    glm::vec3 old_rot, new_rot;
    glm::vec3 old_scale, new_scale;

    void execute() override { /* применить new_* */ }
    void undo()    override { /* применить old_* */ }
};
```

### `cmd_set_field` — изменение поля компонента (из Inspector)

```cpp
struct cmd_set_field : edt::command {
    std::string node_name;
    std::string comp_key;
    std::string field_key;
    boost::json::value old_value;
    boost::json::value new_value;

    void execute() override { /* overrides[field_key] = new_value + on_node_changed */ }
    void undo()    override { /* overrides[field_key] = old_value + on_node_changed */ }
};
```

### `cmd_rename_node`, `cmd_reparent_node`, `cmd_duplicate_node`

Аналогично — снапшот до/после + serialize_and_push.

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

`<desc>` = `command::description()` последней команды.

### История в Console Panel (опционально)

Вкладка "History" в `console_panel` — список последних N команд с возможностью
кликнуть и откатиться до выбранного состояния.

---

## Особые случаи

- **Transform drag** — генерирует одну команду при `mouse release` (не на каждый кадр)
- **Batch операции** — удаление нескольких узлов = одна команда `cmd_delete_batch`
- **Save** — НЕ очищает историю (можно undo после сохранения в рамках сессии)
- **Load level** / **New level** — очищает историю (`command_manager::clear()`)

---

## Критерии готовности

- [ ] `command_manager` реализован с undo/redo стеком (макс. 50 шагов)
- [ ] Ctrl+Z / Ctrl+Y работают в редакторе
- [ ] Add node, Delete node, Transform commit обёрнуты в команды
- [ ] Inspector field change создаёт `cmd_set_field`
- [ ] Меню Edit → Undo/Redo с описанием последней команды
- [ ] Load/New level очищает историю
