# Epics Index

## Themes

| Theme | Folder | Описание |
|---|---|---|
| Editor Features | [editor/](editor/) | UI-панели, инструменты, pipeline ассетов (первое поколение) |
| Desc-Driven Architecture | [desc-driven/](desc-driven/) | Переход к world_desc как источнику правды |
| Prefab-First | [prefab-first/](prefab-first/) | Замена prototype-десков на prefab-иерархию; type-safe редактор |
| Render Migration | [render-migration/](render-migration/) | Переход к data-driven конвейеру: экстракторы → render pass'ы → удаление renderer_3d |
| Infrastructure | [infrastructure/](infrastructure/) | Логгер, профайлер, сборка, инструменты разработки |

---

## All Epics

### Editor Features

| ID | Title | Status |
|---|---|---|
| [EPIC-01](editor/EPIC-01.md) | Gizmo — трансформация объектов в вьюпорте | done |
| [EPIC-02](editor/EPIC-02.md) | Inspector — динамическая инспекция компонентов | done |
| [EPIC-03](editor/EPIC-03.md) | Asset Browser — просмотр и импорт ассетов | done |
| [EPIC-04](editor/EPIC-04.md) | Scene Save/Load — сохранение и управление сценами | done |
| [EPIC-05](editor/EPIC-05.md) | Entity Management — продвинутые операции над сущностями | done |
| [EPIC-06](editor/EPIC-06.md) | Рефакторинг и чистка кодовой базы редактора | done |
| [EPIC-07](editor/EPIC-07.md) | Material & Shader Editor | planned |
| [EPIC-15](editor/EPIC-15.md) | Command Pattern — Undo/Redo | planned |
| [EPIC-22](editor/EPIC-22.md) | Asset Import Pipeline — импорт 3D-моделей в проект | done |
| [EPIC-24](editor/EPIC-24.md) | Async Editor Operations — асинхронные операции редактора | done |
| [EPIC-25](editor/EPIC-25.md) | Prefab Editor — автономное редактирование prefab-ассетов | planned |
| [EPIC-26](editor/EPIC-26.md) | Shader Desc — шейдеры как desc-ресурсы | planned |
| [EPIC-27](editor/EPIC-27.md) | Texture Desc — полноценная система текстурных дескрипторов | planned |
| [EPIC-30](editor/EPIC-30.md) | Editor Code Review — баги, производительность, реорганизация | done |
| [EPIC-34](editor/EPIC-34.md) | Editor Model Importer — расширение и упрощение | planned |
| [EPIC-36](editor/EPIC-36.md) | Editor VFS Protocol — `edt://` для ресурсов редактора | planned |
| [EPIC-40](editor/EPIC-40.md) | Icon Font Integration — иконочный шрифт для UI редактора | planned |
| [EPIC-42](editor/EPIC-42.md) | Workspace & Game Project System — разделение движка и игровых данных | planned |

### Desc-Driven Architecture

| ID | Title | Status |
|---|---|---|
| [EPIC-08](desc-driven/EPIC-08.md) | Desc-Driven Core Infrastructure | done |
| [EPIC-09](desc-driven/EPIC-09.md) | Scene Editing Operations | done |
| [EPIC-10](desc-driven/EPIC-10.md) | Inspector & Hierarchy Redesign | done |
| [EPIC-11](desc-driven/EPIC-11.md) | Editor Camera & Render Targets | done |

### Prefab-First

| ID | Title | Status |
|---|---|---|
| [EPIC-12](prefab-first/EPIC-12.md) | Model Importer → Prefab Output | done |
| [EPIC-13](prefab-first/EPIC-13.md) | Remove Prototype Descs | done |
| [EPIC-14](prefab-first/EPIC-14.md) | Component UI Registry — type-safe editor | done |
| [EPIC-33](prefab-first/EPIC-33.md) | Skinning & Animation Refactoring — префаб-совместимая архитектура | in_progress |

### Infrastructure

| ID | Title | Status |
|---|---|---|
| [EPIC-23](infrastructure/EPIC-23.md) | Logger Migration — spdlog Integration | done |
| [EPIC-28](infrastructure/EPIC-28.md) | Resource Cache — кеш по тегу + типу ресурса | planned |
| [EPIC-29](infrastructure/EPIC-29.md) | Service Layer Refactoring — удаление game_system, единый паттерн сервисов | planned |
| [EPIC-31](infrastructure/EPIC-31.md) | Assert System — информативные ассерты с диалогом и стек-трейсом | planned |
| [EPIC-32](infrastructure/EPIC-32.md) | External Assert Dialog — отдельный процесс для ассерт-диалога (lowest priority) | planned |
| [EPIC-35](infrastructure/EPIC-35.md) | Engine Model Importer Adapter — модернизация для runtime/mods | planned |
| [EPIC-37](infrastructure/EPIC-37.md) | Unit Test Coverage — аудит, исправления, расширение | done |
| [EPIC-38](infrastructure/EPIC-38.md) | res::tag — std::formatter и ostream support | done |
| [EPIC-39](infrastructure/EPIC-39.md) | Performance Profiling Infrastructure — замеры производительности | done |
| [EPIC-41](infrastructure/EPIC-41.md) | CMake Build System — продакшн-качество сборки | planned |
| [EPIC-43](infrastructure/EPIC-43.md) | Memory Allocators — система управления памятью движка | done |
| [EPIC-44](infrastructure/EPIC-44.md) | Memory Profiling & Container Aliases — трекинг памяти и удобные контейнеры | planned |
| [EPIC-46](infrastructure/EPIC-46.md) | Tracy Profiler Integration — внешний профайлер для детального анализа | planned |
| [EPIC-47](infrastructure/EPIC-47.md) | ds::string_id — хешированные идентификаторы строк | planned |

### Render Migration

| ID | Title | Status |
|---|---|---|
| [EPIC-16](render-migration/EPIC-16.md) | Data Contracts — контракты данных рендера | done |
| [EPIC-17](render-migration/EPIC-17.md) | Extractor Layer — слой экстракторов | done |
| [EPIC-18](render-migration/EPIC-18.md) | Render Passes — конкретные проходы рендеринга | done |
| [EPIC-19](render-migration/EPIC-19.md) | Pipeline Wiring — подключение конвейера | done |
| [EPIC-20](render-migration/EPIC-20.md) | Game Loop Migration — миграция игрового цикла | done |
| [EPIC-21](render-migration/EPIC-21.md) | Legacy Renderer Removal — удаление renderer_3d | done |
| [EPIC-45](render-migration/EPIC-45.md) | Render Pipeline Optimization — оптимизация экстракции и рендер пассов | planned |
| [EPIC-48](render-migration/EPIC-48.md) | Skeleton & Skinning Pipeline v2 — desc-driven скелет + GPU batching | planned |

---

## Dependency graph

```
EPIC-08 (Core Infra)
  ├── EPIC-09 (Scene Editing Ops)
  │     └── EPIC-05 (Entity Management)
  │     └── EPIC-15 (Undo/Redo)
  ├── EPIC-10 (Inspector/Hierarchy)
  │     └── EPIC-07 (Material Editor)
  ├── EPIC-11 (Editor Camera)
  └── EPIC-06 (Refactoring)  ← после 08-11

EPIC-08 + EPIC-09 (Prefab Infra)
  └── EPIC-12 (Model Importer → Prefab)
        └── EPIC-13 (Remove Prototype Descs)
EPIC-08 + EPIC-10 (Inspector)
  └── EPIC-14 (Component UI Registry)

EPIC-23 (Logger → spdlog)  ← standalone, no deps

EPIC-24 (Async Editor Ops)  ← standalone

EPIC-12 + EPIC-13 (Prefab Model)
  └── EPIC-33 (Skinning & Animation Refactoring)

EPIC-22 (Asset Import Pipeline)
  └── EPIC-34 (Editor Model Importer — расширение и упрощение)
        └── EPIC-35 (Engine Model Importer Adapter — модернизация)

EPIC-36 (Editor VFS Protocol edt://)  ← standalone, no deps
  └── EPIC-42 (Workspace & Game Project System)  ← depends on EPIC-36

EPIC-37 (Unit Test Coverage)  ← standalone, no deps

EPIC-38 (res::tag formatter)  ← standalone, no deps

EPIC-39 (Performance Profiling)  ← depends on EPIC-23 (Logger) — done
  └── EPIC-46 (Tracy Profiler Integration)  ← depends on EPIC-39

EPIC-40 (Icon Font)  ← standalone, no deps

EPIC-41 (CMake Build System)  ← standalone, no deps
  └── US-41-7 координируется с EPIC-34 US-34-8 (assimp_importer)

EPIC-43 (Memory Allocators)  ← standalone, мотивация из EPIC-39 (Profiling)
  ├── EPIC-44 (Memory Profiling & Container Aliases)  ← depends on EPIC-43
  └── EPIC-45 (Render Pipeline Optimization)  ← depends on EPIC-43, EPIC-39

EPIC-47 (ds::string_id)  ← standalone, no deps

EPIC-33 (Skinning & Animation Refactoring)
  └── EPIC-48 (Skeleton & Skinning Pipeline v2)  ← depends on EPIC-33, EPIC-45

EPIC-25 (Prefab Editor)  ← после EPIC-12, 13, 14, 22
EPIC-26 (Shader Desc)    ← после EPIC-08
  └── EPIC-07 (Material Editor)  ← после EPIC-26

EPIC-16 (Data Contracts)
  └── EPIC-17 (Extractor Layer)
        └── EPIC-18 (Render Passes)
              └── EPIC-19 (Pipeline Wiring)
                    └── EPIC-20 (Game Loop Migration)
                          └── EPIC-21 (Legacy Renderer Removal)
```
