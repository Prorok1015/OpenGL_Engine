# EPIC-08: Desc-Driven Core Infrastructure

**Theme:** desc-driven
**Status:** done
**Depends on:** —
**Blocks:** EPIC-09, EPIC-10, EPIC-11

## Цель

Заложить фундамент: редактор работает с мутируемой копией `world_desc`,
изменения пушатся в resource system под memory-тегом, hot-reload обновляет ECS.
`scn::level` получает метод перезагрузки одного мира.

---

## US-19: In-memory world_desc state

**Файлы:** `edt_editor_system.h/.cpp`

### Что делать

Добавить в `editor_system`:

```cpp
// Мутируемая копия активного world_desc (источник правды редактора)
scn::world_desc m_world_desc;

// Тег оригинального файла (res://levels/...) — только для сохранения
res::tag m_level_tag;

// Тег рабочей копии (memory://editor/...) — через него живёт ECS
res::tag m_editor_tag;
```

### Поток загрузки уровня

```
load_level(path):
  1. оригинальный_тег = res::tag::make(path)
  2. res_handle<level_desc> orig = require_sync<level_desc>(оригинальный_тег)
  3. m_world_desc = копия world_desc из orig (world_desc::copy_to)
  4. m_editor_tag = res::tag{ "memory://editor/" + stem(path) + ".desc" }
  5. serialize_and_push(m_world_desc, m_editor_tag)   // store + signal_changed
  6. level_manager->load(m_editor_tag)                // ECS живёт из копии
  7. m_level_tag = оригинальный_тег                   // для Save
  8. setup_world_reload_watcher(m_editor_tag)
```

### `new_level()`

```
new_level():
  1. m_world_desc = default world_desc (name="3d_scene", дефолтные systems)
  2. m_editor_tag = "memory://editor/untitled.desc"
  3. serialize_and_push → level_manager->load
  4. m_level_tag = {}  // нет оригинала, пока не сохранён
```

### Helper `serialize_and_push`

```cpp
void serialize_and_push(const scn::world_desc& wd, const res::tag& tag) {
    boost::json::object obj;
    obj["__type"] = "world_desc";   // serialize() не пишет __type
    wd.serialize(obj);
    res::get_system().store(tag, desc_system.serialize_to_bytes(json::value{obj}));
    res::get_system().signal_changed(tag);
}
```

### Watcher после load

После `level_manager->load(m_editor_tag)` регистрируем watcher:
```cpp
res::get_system().watch(m_editor_tag, this, [this](const res::tag& tag) {
    on_editor_world_reloaded();
});
```
`on_editor_world_reloaded()` — синхронизирует панели с новым состоянием ECS
(см. EPIC-11 для editor camera).

---

## US-20: `scn::level::reload_world()`

**Файлы:** `core/engine/scene/level/scn_level.h/.cpp`

### Проблема

Нет метода перезагрузки одного мира без сброса всего уровня.
`load_world_from_desc` захардкожен на `world_id=0` и не убирает старый мир.

### Новый метод

```cpp
// В scn::level:
void reload_world(
    const std::string_view name,
    const world_desc& desc,
    ecs::system_factory& sf,
    scn::ecs_assembler& asm);
```

Реализация:
```
1. auto& old_world = get_world(name)
2. uint32_t old_id = old_world.world_id()  // нужно добавить геттер в scn::world
3. if (m_hot_reload_manager) m_hot_reload_manager->detach_registry(old_world.state())
4. m_worlds.erase(name); m_worlds_by_id.erase(old_id)
5. scn::world& w = create_world(name, old_id)   // пересоздаём с тем же id
6. for (sys : desc.get_systems()) sf.create_system(sys, w.state(), organizer())
7. entt::entity e = w.state().create()
8. asm.spawn_from_desc(w.state(), e, desc, name)
9. mark_systems_graphs_dirty()
```

### Добавить в `scn::world`

```cpp
uint32_t get_id() const { return m_world_id; }
```

---

## Критерии готовности

- [ ] `load_level()` загружает ECS через memory-копию, оригинальный файл не трогается
- [ ] `new_level()` работает через тот же путь
- [ ] `signal_changed(m_editor_tag)` пересоздаёт ECS через hot-reload
- [ ] `scn::level::reload_world()` заменяет один мир без сброса остальных
- [ ] `scn::world::get_id()` добавлен
