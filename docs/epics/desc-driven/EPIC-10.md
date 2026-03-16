# EPIC-10: Inspector & Hierarchy Redesign

**Theme:** desc-driven
**Status:** done
**Depends on:** EPIC-08
**Blocks:** —

## Цель

`inspector_panel` и `scene_hierarchy_panel` перестают работать с `entt::registry`.
Иерархия отображает `prefab_node` дерево из `world_desc`.
Inspector отображает поля `prefab_comp_node` напрямую.

---

## US-26: Hierarchy panel работает с `world_desc*`

**Файлы:** `edt_scene_hierarchy_panel.h/.cpp`

### Изменение интерфейса

```cpp
// Убрать:
void set_registry(std::shared_ptr<entt::registry> registry);

// Добавить:
void set_world_desc(scn::world_desc* desc);
void set_on_node_selected(std::function<void(scn::prefab_desc::prefab_node*)> cb);
void set_on_delete_node(std::function<void(const std::string& name)> cb);
void set_on_create_node(std::function<void(const std::string& type_name)> cb);
```

### `on_render()` — рекурсивный обход дерева

```cpp
void draw_node(scn::prefab_desc::prefab_node& node) {
    // prefix по типам компонентов:
    // node.components.count("camera_desc")          → "[C] "
    // node.components.count("directional_light_desc")→ "[L] "
    // node.components.count("skin_prototype_desc")   → "[M] "
    // node.components.count("skybox_desc")           → "[S] "
    // иначе                                          → ""

    // TreeNode по node.name
    // Click → set_on_node_selected(&node)
    // Context menu → Rename, Add Child, Delete
    // Recurse node.children
}
```

### Мировые вкладки

Остаются. Смена вкладки → `editor_system` вызывает `set_world_desc(other_world)`.
(Пока поддержка одного world_desc — мультимир в след. итерации.)

### Убрать

- `m_registry`
- `destroy_entity()` (прямой доступ к ECS)
- Итерацию `m_registry->storage<entt::entity>()`
- `get_type_prefix()` через ECS-компоненты → заменить на проверку `components` map

---

## US-27: Inspector panel работает с `prefab_node*`

**Файлы:** `edt_inspector_panel.h/.cpp`

### Изменение интерфейса

```cpp
// Убрать:
void set_selected_entity(ecs::entity);
void set_registry(shared_ptr<entt::registry>);

// Добавить:
void set_selected_node(scn::prefab_desc::prefab_node* node);
void set_on_node_changed(std::function<void()> cb);
```

### `on_render()` — секции

**1. Transform** (из typed полей `prefab_node`, не из ECS):
```cpp
if (ImGui::DragFloat3("Position", &node->position.x, 0.01f)) changed = true;
// rotation: DragFloat3, degrees
// scale:    DragFloat3
```

**2. Components** — для каждого `prefab_comp_node` в `node->components`:
```cpp
for (auto& [key, comp] : node->components) {
    if (ImGui::CollapsingHeader(comp.type_name.c_str())) {
        auto it = m_renderers.find(comp.type_name);
        if (it != m_renderers.end())
            changed |= it->second(comp);
        else
            render_generic_json(comp.overrides);   // fallback
    }
}
```

**3. Add Component** — кнопка открывает popup с известными типами.
Добавляет пустой `prefab_comp_node` в `node->components`, вызывает `on_node_changed`.

---

## US-28: Desc component renderers

**Файлы:** `edt_component_renderers.hpp`

### Новый тип рендерера

```cpp
using desc_comp_renderer =
    std::function<bool(scn::prefab_desc::prefab_comp_node&)>;
```

Регистрация:
```cpp
void register_desc_renderer(
    inspector_panel& panel,
    const std::string& type_name,
    desc_comp_renderer renderer);
```

Рендеры читают/пишут поля через `comp.overrides` (boost::json::object).
Defaults берутся из кода (не из desc, чтобы избежать лишних require_sync).

### Реализовать рендереры для

| type_name | Поля |
|---|---|
| `camera_desc` | fov (float), near_distance (float), far_distance (float), texture (tag string) |
| `directional_light_desc` | direction (vec3), diffuse (color), ambient (color), specular (color) |
| `skybox_desc` | material (tag string) |
| `skin_prototype_desc` / `skinning_prototype_desc` | (read-only имя файла из parent_desc тега) |

### `render_generic_json` — fallback

```cpp
void render_generic_json(boost::json::object& obj) {
    for (auto& [key, val] : obj) {
        if (val.is_double()) { ... DragFloat ... }
        else if (val.is_string()) { ... InputText ... }
        else if (val.is_array() && val.as_array().size() == 3) { ... DragFloat3 ... }
        // и т.д.
    }
}
```

---

## Критерии готовности

- [ ] `scene_hierarchy_panel` не держит `m_registry`, итерирует `prefab_node` дерево
- [ ] `inspector_panel` не держит `m_registry`, отображает поля `prefab_comp_node`
- [ ] Transform редактируется в Inspector → `on_node_changed` → hot-reload
- [ ] Рендереры для camera, light, skybox, skin prototype работают
- [ ] Generic JSON fallback работает для незнакомых типов
- [ ] Add Component в Inspector добавляет `prefab_comp_node` в узел
