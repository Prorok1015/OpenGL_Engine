# EPIC-09: Scene Editing Operations

**Theme:** desc-driven
**Status:** done
**Depends on:** EPIC-08
**Blocks:** —

## Цель

Все структурные операции над сценой (добавить/удалить объект, изменить transform,
сохранить) идут через мутацию `m_world_desc` + `serialize_and_push` + hot-reload.
Прямые вызовы `registry->create()` / `registry->emplace()` для пользовательских
объектов устранены.

---

## US-21: Multi-type asset drop handler

**Файлы:** `edt_editor_system.cpp` (drop callback в `init()`)

### Проблема

```cpp
// Сейчас — один тип:
auto handle = res::get_system().require_sync<scn::skinning_prototype_desc>(tag);
```

### Решение

Добавить helper `read_desc_type(tag)` — синхронно читает файл как text,
парсит JSON, возвращает значение поля `"__type"`:

```cpp
std::string read_desc_type(const res::tag& tag) {
    auto raw = res::get_system().require_sync<res::text_resource>(tag);
    if (!raw.is_ready()) return "";
    auto obj = boost::json::parse(raw->text).as_object();
    if (!obj.contains("__type")) return "";
    return boost::json::value_to<std::string>(obj.at("__type"));
}
```

Dispatch в drop callback:

| `__type` в файле | Что добавляем в `world_desc.root.children` |
|---|---|
| `skin_prototype_desc` / `skinning_prototype_desc` | `{ "__type": type, "__parent": tag }` |
| `skybox_desc` | `{ "__type": "skybox_desc", "material": tag }` |
| `camera_desc` | `{ "__type": "camera_desc" }` |
| `directional_light_desc` | `{ "__type": "directional_light_desc" }` |
| `material_desc` | warning в консоль (нет контейнера) |
| нет `__type` (glb/obj/fbx) | `{ "__type": "skin_prototype_desc", "__parent": tag }` |

После добавления узла → `serialize_and_push(m_world_desc, m_editor_tag)`.

Имя нового узла = stem(filename) + суффикс если уже занято.

---

## US-22: Create entity via desc

**Файлы:** `edt_editor_system.cpp`, `edt_scene_hierarchy_panel.h/.cpp`

### Изменения в панели

```cpp
// Убрать прямое создание registry:
// entt::entity e = m_registry->create(); ...

// Добавить callback:
void set_on_create_entity(std::function<void(const std::string& type_name)> cb);
```

Кнопка `"+ New Entity"` открывает popup с выбором типа:
`Empty`, `Camera`, `Directional Light`, `Skybox`, `Skinned Model`.

### В `editor_system`

```cpp
void create_entity(const std::string& type_name) {
    std::string key = make_unique_key(type_name, m_world_desc.get_root());
    scn::prefab_desc::prefab_node node;
    node.name = key;
    if (type_name != "Empty") {
        scn::prefab_desc::prefab_comp_node comp;
        comp.type_name = type_name;
        node.components[type_name] = std::move(comp);
    }
    m_world_desc.get_root().children.push_back(std::move(node));
    serialize_and_push(m_world_desc, m_editor_tag);
}
```

`make_unique_key` — генерирует `"Camera"`, `"Camera_1"`, `"Camera_2"` и т.д.

---

## US-23: Delete entity via desc

**Файлы:** `edt_editor_system.cpp`, `edt_scene_hierarchy_panel.h/.cpp`

### Изменения в панели

```cpp
// Убрать прямой destroy_entity()
// Добавить:
void set_on_delete_entity(std::function<void(const std::string& name)> cb);
```

Context menu "Delete" → вызывает callback с `node->name`.

### В `editor_system`

```cpp
void delete_entity(const std::string& name) {
    remove_node_by_name(m_world_desc.get_root().children, name);  // рекурсивно
    serialize_and_push(m_world_desc, m_editor_tag);
}
```

---

## US-24: Transform commit → desc sync

**Файлы:** `edt_editor_system.cpp`, `edt_viewport_panel.h/.cpp`, `edt_transform_gizmo.hpp`

### Стратегия

- Во время drag гизмо: обновляет ECS напрямую (real-time, без hot-reload)
- По завершении drag (mouse release): коммит в `m_world_desc` → без hot-reload

  > Reload при каждом commit будет уничтожать и пересоздавать сущности.
  > Это приемлемо: commit = конец движения, не каждый кадр.
  > После reload: re-find selected node по `name_component::name`.

### Изменения в гизмо

```cpp
// Добавить callback в viewport_panel:
void set_on_transform_committed(
    std::function<void(const std::string& entity_name,
                       glm::vec3 pos, glm::vec3 rot, glm::vec3 scale)> cb);
```

Вызывается из `edt_transform_gizmo.hpp` когда `ImGuizmo::IsUsing()` переходит
из `true` в `false`.

### В `editor_system`

```cpp
void on_transform_committed(const std::string& name,
                             glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
    auto* node = find_node_by_name(m_world_desc.get_root().children, name);
    if (!node) return;
    node->position = pos;
    node->rotation = rot;   // degrees
    node->scale    = scale;
    // НЕ вызываем serialize_and_push — ECS уже актуален, JSON обновлён для Save
    // m_is_dirty = true  (если нужен dirty-флаг)
}
```

При сохранении JSON берётся из `m_world_desc` который уже содержит новый transform.

---

## US-25: Simplified save

**Файлы:** `edt_editor_system.cpp`

### Сейчас

`save_level()` вызывает `edt::serialize_level(level_name, worlds_data)` —
обходит ECS и реконструирует JSON. Хрупко и требует отдельного сериализатора.

### После

```cpp
void do_save(const std::filesystem::path& abs_path, const std::string& level_name) {
    // 1. Синхронизировать transform из ECS в m_world_desc
    //    (на случай если commit не вызывался — например при undo)
    sync_ecs_transforms_to_desc();

    // 2. Собрать level JSON
    boost::json::object world_obj;
    world_obj["__type"] = "world_desc";
    m_world_desc.serialize(world_obj);

    boost::json::object level_obj;
    level_obj["__type"]  = "level_desc";
    level_obj["name"]    = level_name;
    level_obj["worlds"]  = boost::json::array{ world_obj };

    // 3. Записать оригинальный файл
    std::ofstream out(abs_path);
    out << boost::json::serialize(level_obj);

    // 4. m_level_tag обновлён, m_editor_tag продолжает жить
}
```

`sync_ecs_transforms_to_desc()` — итерирует world registry, для каждой
`name_component` находит `prefab_node` в `m_world_desc` и обновляет
`position/rotation/scale` из `local_transform`.

### После реализации US-25

`edt_scene_serializer.hpp` можно пометить deprecated и удалить.

---

## Критерии готовности

- [ ] Drop любого `.desc` типа добавляет нужный узел в сцену через hot-reload
- [ ] Drop `.glb/.obj/.fbx` создаёт `skin_prototype_desc` узел
- [ ] `"+ New Entity"` с выбором типа работает без прямого `registry->create()`
- [ ] Delete entity работает через desc + hot-reload
- [ ] Transform гизмо коммитит в `m_world_desc.prefab_node` при отпускании мыши
- [ ] Save пишет `m_world_desc` на диск без обхода ECS
- [ ] `edt_scene_serializer.hpp` удалён
