# EPIC-02: Inspector — динамическая инспекция компонентов

**Theme:** editor
**Status:** done

## Реализовано

- `inspector_panel` с системой регистрации рендереров компонентов
- Dispatch по `entt::type_hash` — каждый тип компонента имеет свой рендерер
- Рендереры реализованы для: `local_transform`, `mesh_node_desc`, `directional_light`,
  `camera_component`, `mouse_controller_component`, `scn::name_component`
- Рендерер `local_transform` — DragFloat3 для position/rotation/scale
- Рендерер `directional_light` — color pickers для diffuse/ambient/specular, direction
- Рендерер `camera_component` — fov/near/far sliders
- Fallback: перечисление имён компонентов без редактирования

## Ключевые файлы

- `Editor/code/editor_system/panels/edt_inspector_panel.h/.cpp`
- `Editor/code/editor_system/edt_component_renderers.hpp`

## Устаревает при

Реализации EPIC-10 (Inspector работает с `prefab_node*` вместо ECS-компонентов).
