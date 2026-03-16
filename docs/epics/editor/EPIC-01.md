# EPIC-01: Gizmo — трансформация объектов в вьюпорте

**Theme:** editor
**Status:** done

## Реализовано

- ImGuizmo интегрирован в render pass вьюпорта
- `edt_transform_gizmo.hpp` — отрисовка гизмо поверх scene render target
- `edt_guizmo.hpp` — orientation cube gizmo в углу вьюпорта
- Клавиши T/R/S переключают режим (Translate / Rotate / Scale)
- Гизмо применяет трансформацию к `scn::local_transform` выбранной сущности
- Матрицы view/proj берутся из первой `camera_component` в активном registry

## Ключевые файлы

- `Editor/code/editor_system/edt_transform_gizmo.hpp`
- `Editor/code/editor_system/edt_guizmo.hpp`
- `Editor/code/editor_system/panels/edt_viewport_panel.cpp`
