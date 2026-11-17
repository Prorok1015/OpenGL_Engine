# Фикстура `migration_v1` (EPIC-58 S0, #750)

Маленький проект старых версий для скана и миграции (S2–S8). Тесты работают с копией (`test_util::fixture_copy("migration_v1")`, `tests/test_fixture_project.hpp`); оригинал не меняется — это проверяет `prj_migration_fixture_tests.cpp`.

| Файл | Что в нём | Класс скана / зачем |
|---|---|---|
| `project.snkproj` | `format_version` 1, `engine_version` 0.1.0 | проект старше движка; формат 1 — до S4 #754 (текущий — 2: `migrated_with`, `applied_steps`), его поднимает шаг `engine.manifest_format` |
| `res/levels/old.desc` | `level_desc` v1, внутри мир v1 с камерой v1 | устаревший |
| `res/worlds/w_v1.desc` | `world_desc` v1: дети объектом по имени, камера v1 с позой | устаревший |
| `res/worlds/w_v2.desc` | `world_desc` v2: массив узлов с `__id`, вложенный инстанс v2 | устаревший |
| `res/worlds/w_v3.desc` | `world_desc` v3: камера v1 в узле и override с **частичной** позой камеры | устаревший; шаг 3→4 предупреждает о потере (S1 #751) |
| `res/prefabs/p_v1..p_v3.prefab.desc` | `prefab_desc` v1, v2, v3 | устаревшие |
| `res/cameras/cam_v1.desc` | `camera_desc` v1 вне узла | устаревший; шаг 1→2 отбрасывает позу (предупреждение) |
| `res/module/unknown.desc` | тип `migration_fixture_module_desc`, не зарегистрирован | `unknown_type` (S2 #752, S9b #764); сейчас проходит молча как v1 |
| `res/newer.desc` | `world_desc` v99 | `newer` |
| `res/broken.desc` | битый JSON | `broken` |
| `res/textures/plain.png` | PNG 1×1 без `.resource` | `missing_resource` (S2, после формата `.resource` S57-1 #755) |
| `res/textures/with_meta.png` + `.resource` | PNG 1×1 и сайдкар `resource_meta` версии 1 | текущий; тестовый шаг v1→v2 (S4 #754) делает его устаревшим и мигрирует |
