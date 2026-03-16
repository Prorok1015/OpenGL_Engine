# Theme: Desc-Driven Editor Architecture

## Суть

Инвертировать модель данных редактора: вместо "ECS как источник правды" перейти к
"`world_desc` как источник правды, ECS — его runtime-представление".

## Текущая архитектура (проблемы)

```
desc ──load──► ECS  ◄── редактор читает/пишет напрямую
                │
                └── save ──► serialize(ECS→JSON) ──► файл
```

- Drop handler знает только один тип (`skinning_prototype_desc`) — баг
- Прямая запись в registry обходит desc-систему (нет hot-reload для правок редактора)
- Сохранение через `edt_scene_serializer` — хрупкое ECS→JSON преобразование
- Camera entity уничтожается при hot-reload
- Inspector работает с ECS-компонентами, а не с типизированными desc-полями

## Целевая архитектура

```
world_desc  ◄── редактор читает/пишет напрямую
    │
    ├── store(memory://editor/...) + signal_changed()
    │
    └──► hot-reload ──► ECS  ──► рендерер
```

- `world_desc` (mutable копия) — единственный источник правды в редакторе
- ECS пересоздаётся автоматически через hot-reload при каждом изменении
- Сохранение = serialize(`world_desc`) → файл, без обхода ECS
- Inspector работает с `prefab_node*` и `prefab_comp_node&` (typed fields)
- Editor camera инжектируется в мир после каждого hot-reload, не входит в desc

## Epics в этой теме

| ID | Название | Зависит от |
|---|---|---|
| [EPIC-08](EPIC-08.md) | Desc-Driven Core Infrastructure | — |
| [EPIC-09](EPIC-09.md) | Scene Editing Operations | EPIC-08 |
| [EPIC-10](EPIC-10.md) | Inspector & Hierarchy Redesign | EPIC-08 |
| [EPIC-11](EPIC-11.md) | Editor Camera & Render Targets | EPIC-08 |

## Ключевые структуры данных

### `prefab_desc::prefab_node` — узел сцены

```cpp
struct prefab_node {
    std::string name;
    glm::vec3 position{ 0.0f };
    glm::vec3 rotation{ 0.0f };
    glm::vec3 scale{ 1.0f };
    std::unordered_map<std::string, prefab_comp_node> components;
    std::vector<prefab_node> children;
};
```

### `prefab_desc::prefab_comp_node` — компонент узла

```cpp
struct prefab_comp_node {
    std::string type_name;                          // "camera_desc", "skybox_desc", ...
    res::res_handle<desc::desc_base> parent_desc;   // __parent ресурс (если есть)
    boost::json::object overrides;                  // локальные переопределения полей
};
```

### `world_desc` — весь мир

`world_desc` наследует `prefab_desc`, имеет `root: prefab_node`.
Дерево сцены: `root.children` — верхнеуровневые сущности.

## Инварианты после рефакторинга

1. `editor_system` никогда не вызывает `registry->create()` / `registry->emplace()` для
   пользовательских объектов — только через `world_desc` мутацию + hot-reload
2. `inspector_panel` не держит `shared_ptr<entt::registry>` — только `prefab_node*`
3. `scene_hierarchy_panel` не держит `shared_ptr<entt::registry>` — только `world_desc*`
4. `viewport_panel` держит registry read-only (для камеры и гизмо)
5. Editor camera пересоздаётся после каждого hot-reload; её состояние сохраняется
