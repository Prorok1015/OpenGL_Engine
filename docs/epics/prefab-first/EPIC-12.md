# EPIC-12: Model Importer → Prefab Output

**Theme:** prefab-first
**Status:** done
**Depends on:** EPIC-08, EPIC-09

## Цель

Перевести `model_importer_adapter` с вывода `skin_prototype_desc` на вывод `prefab_desc`.
Модель, загруженная через Assimp, должна сразу стать набором `prefab_node` с компонентными десками
(`mesh_node_desc`, `skinning_desc`, `bone_desc`, `animations_desc`), а не монолитным прототипом.

**Мотивация:**
- `skin_prototype_desc` — монолит со своей логикой спавна (`load_prototype`), не вписывающийся
  в desc-driven архитектуру.
- Редактор не может добавить/удалить компоненты модели через инспектор, пока они внутри прототипа.
- Уже созданы отдельные компонентные дески: `mesh_node_desc`, `skinning_desc`, `bone_desc`,
  `animations_desc` — надо только использовать их.

---

## Текущий формат вывода (`skin_prototype_desc`)

```json
{
  "__type": "skin_prototype_desc",
  "geometry": { "__type": "geometry_desc", ... },
  "tree": {
    "name": "RootNode",
    "local": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
    "children": [
      {
        "name": "Mesh",
        "mesh": {
          "vx_begin": 0, "vx_end": 100,
          "ind_begin": 0, "ind_end": 200,
          "material": { "__type": "material_desc", ... },
          "weights": [...]
        }
      }
    ],
    "bone": { "name": "...", "offset_matrix": [...] }
  },
  "animations": { ... }
}
```

## Целевой формат вывода (`prefab_desc`)

```json
{
  "__type": "prefab_desc",
  "root": {
    "name": "RootNode",
    "position": [0, 0, 0],
    "rotation": [0, 0, 0],
    "scale": [1, 1, 1],
    "components": {
      "animations_desc": {
        "__type": "animations_desc",
        "animations": { ... }
      }
    },
    "children": [
      {
        "name": "Mesh",
        "components": {
          "mesh_node_desc": {
            "__type": "mesh_node_desc",
            "geometry": "memory://path/model.geom.desc",
            "vx_begin": 0, "vx_end": 100,
            "ind_begin": 0, "ind_end": 200,
            "material": { "__type": "material_desc", ... }
          },
          "skinning_desc": {
            "__type": "skinning_desc",
            "bone_count": 32
          }
        }
      },
      {
        "name": "Bone_01",
        "components": {
          "bone_desc": {
            "__type": "bone_desc",
            "offset_matrix": [[...],[...],[...],[...]],
            "index": 0
          }
        }
      }
    ]
  }
}
```

---

## US-12-1: Декомпозиция aiMatrix4x4 → TRS

**Файлы:** `scn_model_importer_adapter.cpp`

`prefab_node` хранит `position`, `rotation` (Эйлер, градусы), `scale` вместо `local` матрицы.
Реализовать `decompose_aimatrix(const aiMatrix4x4&) → {pos, rot, scale}` с помощью `glm::decompose`.

Замечание: `glm::decompose` требует `<glm/gtx/matrix_decompose.hpp>`. Поворот возвращается как кватернион — конвертировать в Эйлер через `glm::eulerAngles(quat)`, затем `glm::degrees(...)`.

**AC:**
- `decompose_aimatrix` корректно конвертирует простые трансформации (трансляция, масштаб)
- Для узлов с ненулевым поворотом Эйлер round-trip с погрешностью < 0.01°

---

## US-12-2: `process_node` → вывод `prefab_node` JSON

**Файлы:** `scn_model_importer_adapter.cpp`

Заменить текущий `process_node` (выводящий `{name, local, children, bone, mesh}`),
на функцию `build_prefab_node`, выводящую:
```json
{
  "name": "...",
  "position": [...], "rotation": [...], "scale": [...],
  "components": { ... },
  "children": [...]
}
```

Логика маппинга:
- `aiNode` с мешами → узел с `mesh_node_desc` + (если есть веса) `skinning_desc`
- `aiNode` найден в `scene->findBone` → узел с `bone_desc`
- Корневой узел, если есть анимации → `animations_desc` в компонентах

**AC:**
- `build_prefab_node` рекурсивно обходит дерево aiScene
- Каждый узел с мешами имеет `mesh_node_desc`
- Каждый bone-узел имеет `bone_desc`

---

## US-12-3: Изменить корневой тип адаптера на `prefab_desc`

**Файлы:** `scn_model_importer_adapter.cpp`

```cpp
jsdata["__type"] = "prefab_desc";
// jsdata["tree"] → jsdata["root"] = build_prefab_node(...)
// geometry сохраняется отдельно и ссылается через тег
```

Геометрия хранится как отдельный `geometry_desc` в памяти (уже так сейчас), ссылка
вставляется в каждый `mesh_node_desc.geometry`.

**AC:**
- Адаптер регистрирует ресурс как `prefab_desc` а не `skin_prototype_desc`
- `desc_system.create_instance("prefab_desc", ...)` возвращает рабочий объект

---

## US-12-4: Удалить spawner `skin_prototype_desc` из edt_cr_skin

**Файлы:** `Editor/code/editor_system/edt_component_renderers/edt_cr_skin.cpp`

После перехода на `prefab_desc` инспектор больше не показывает `skin_prototype_desc` /
`skinning_prototype_desc` компонентов — удалить их из registry.

Вместо этого зарегистрировать рендереры для `skinning_desc` и `bone_desc` (read-only UI).

**AC:**
- Из `component_ui_registry` удалены `skin_prototype_desc`, `skinning_prototype_desc`
- Добавлены read-only рендереры для `skinning_desc` (показывает `bone_count`) и `bone_desc`

---

## US-12-5: Тест с существующими моделями

**Файлы:** `Editor/res/objects/` (backpack, getaur, anim_cube, luke)

Проверить визуально, что модели загружаются и рендерятся корректно.

**AC:**
- backpack.obj рендерится как раньше
- getaur/scene.gltf рендерится со скелетом (если скелет был виден раньше)
- Инспектор показывает компоненты mesh_node, bone, skinning на соответствующих узлах

---

## Критерии готовности

- [ ] `model_importer_adapter` выводит `prefab_desc`
- [ ] Дерево узлов модели представлено как `prefab_node` иерархия
- [ ] Узлы меша имеют `mesh_node_desc` и (при наличии весов) `skinning_desc`
- [ ] Bone-узлы имеют `bone_desc`
- [ ] Анимации хранятся в `animations_desc` на корневом узле
- [ ] Модели из `Editor/res/objects/` загружаются и рендерятся корректно
- [ ] `skin_prototype_desc` больше не используется в `model_importer_adapter`
