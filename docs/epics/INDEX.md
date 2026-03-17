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
| [EPIC-05](editor/EPIC-05.md) | Entity Management — продвинутые операции над сущностями | in_progress |
| [EPIC-06](editor/EPIC-06.md) | Рефакторинг и чистка кодовой базы редактора | done |
| [EPIC-07](editor/EPIC-07.md) | Material & Shader Editor | planned |
| [EPIC-15](editor/EPIC-15.md) | Command Pattern — Undo/Redo | planned |
| [EPIC-22](editor/EPIC-22.md) | Asset Import Pipeline — импорт 3D-моделей в проект | planned |
| [EPIC-24](editor/EPIC-24.md) | Async Editor Operations — асинхронные операции редактора | planned |

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

### Infrastructure

| ID | Title | Status |
|---|---|---|
| [EPIC-23](infrastructure/EPIC-23.md) | Logger Migration — spdlog Integration | done |

### Render Migration

| ID | Title | Status |
|---|---|---|
| [EPIC-16](render-migration/EPIC-16.md) | Data Contracts — контракты данных рендера | planned |
| [EPIC-17](render-migration/EPIC-17.md) | Extractor Layer — слой экстракторов | planned |
| [EPIC-18](render-migration/EPIC-18.md) | Render Passes — конкретные проходы рендеринга | planned |
| [EPIC-19](render-migration/EPIC-19.md) | Pipeline Wiring — подключение конвейера | planned |
| [EPIC-20](render-migration/EPIC-20.md) | Game Loop Migration — миграция игрового цикла | planned |
| [EPIC-21](render-migration/EPIC-21.md) | Legacy Renderer Removal — удаление renderer_3d | planned |

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
  US-24-1 → US-24-2, US-24-3, US-24-4 → US-24-5 → US-24-6

EPIC-16 (Data Contracts)
  └── EPIC-17 (Extractor Layer)
        └── EPIC-18 (Render Passes)
              └── EPIC-19 (Pipeline Wiring)
                    └── EPIC-20 (Game Loop Migration)
                          └── EPIC-21 (Legacy Renderer Removal)
```
