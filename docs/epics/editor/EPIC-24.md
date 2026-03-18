# EPIC-24 — Async Editor Operations

**Status:** done
**Theme:** Editor Features
**Priority:** high

## Motivation

Сейчас все тяжёлые операции в редакторе блокируют UI-поток:

| Операция | Блокирует | Время |
|----------|-----------|-------|
| Импорт модели (Assimp) | UI freeze | 1–10 сек |
| `serialize_and_push()` | UI freeze на каждый edit | 50–500 мс |
| `load_level()` | UI freeze | 0.5–3 сек |
| Export to project | UI freeze | 0.5–2 сек |
| Asset drop → `require_sync` | UI micro-freeze | 50–200 мс |

Resource system уже имеет async-инфраструктуру (`require<T>`, `.then()`, `warmup`, `async_adapter_worker`),
но editor использует `require_sync` везде и вызывает `reload_world` синхронно.

Дополнительная проблема: `serialize_and_push()` вызывает `m_res.store()` который пишет на диск
и провоцирует `file_watcher` → `hot_reload_manager` → повторный reload → бесконечный цикл
при создании material override ресурсов.

## Goals

1. Убрать все UI-freeze при операциях редактора
2. Разорвать цикл store → file_watcher → reload в `serialize_and_push`
3. Импорт моделей — в фоновом потоке с прогресс-баром
4. Загрузка уровней — с loading overlay, без блокировки UI
5. Экспорт — фоновый с progress callback

## Non-Goals

- Многопоточный ECS (assembly остаётся в main thread, но вызывается отложенно)
- Async рендеринг (отдельная тема)
- Streaming уровней (load/unload зон)

## Current Architecture

```
require_sync<T>(tag)  ─── блокирует до ready ──→ adapter_worker thread
require<T>(tag)       ─── возвращает handle    ──→ .then(cb) когда ready
warmup<T>(tag)        ─── require + pin cache  ──→ не используется в editor

serialize_and_push():
  build_level_json()         ← CPU, синхронно
  m_res.store(tag, bytes)    ← пишет на диск → file_watcher → reload loop!
  reload_world(desc)         ← пересоздаёт ECS, синхронно
  inject_editor_camera()     ← восстанавливает камеру
```

## User Stories

### US-24-1: Убрать store из serialize_and_push
**Status:** done
**Priority:** critical (блокер для остальных задач)

Проблема: `m_res.store(m_editor_tag, ...)` пишет level JSON на диск при каждом редактировании.
`file_watcher` обнаруживает изменение → `hot_reload_manager` → повторный `reload_world` →
assembly создаёт новые material overrides → `store` → цикл.

Решение:
- Убрать `m_res.store()` из `serialize_and_push()`
- `reload_world()` уже получает `active_world_desc()` напрямую, resource system ему не нужен
- Запись на диск — только в `save_level()` / `write_level_to_disk()` (явное сохранение)
- Опционально: `m_res.store()` без записи на диск (in-memory only) для кеша

**Файлы:**
- `Editor/code/editor_system/edt_editor_system.cpp` — `serialize_and_push()`

### US-24-2: Async model import с прогрессом
**Status:** done
**Priority:** high

Проблема: `model_importer::import()` — полностью синхронный: Assimp parse → geometry → prefab → store.
Блокирует UI на 1–10 секунд.

Решение:
- `import()` возвращает `std::future<import_result>` или аналог
- Запуск в отдельном потоке (`std::async` или через `adapter_worker`)
- UI показывает progress modal (spinning + текст стадии)
- `show_file_dialog()` опрашивает статус каждый кадр
- По готовности — добавить prefab в сцену и `serialize_and_push()`

Стадии для прогресса:
1. Reading file...
2. Parsing geometry...
3. Processing materials...
4. Building prefab...
5. Done

**Файлы:**
- `Editor/code/editor_system/edt_model_importer.h/cpp`
- `Editor/code/editor_system/edt_editor_system.cpp` — `show_file_dialog()`

### US-24-3: Async export с прогрессом
**Status:** done
**Priority:** medium

Проблема: `asset_exporter::export_to_project()` — синхронный: fetch → parse → remap → write.

Решение:
- `export_to_project()` запускается в фоновом потоке
- Возвращает handle/future
- `asset_export_dialog` показывает progress bar (N / total files)
- По завершении — callback в main thread

**Файлы:**
- `Editor/code/editor_system/edt_asset_exporter.h/cpp`
- `Editor/code/editor_system/edt_asset_export_dialog.h/cpp`

### US-24-4: Async level load с loading overlay
**Status:** done
**Priority:** medium

Проблема: `load_level()` вызывает `require_sync<level_desc>` + `switch_to_world` + `serialize_and_push`.

Решение:
- `require<level_desc>(tag)` вместо `require_sync`
- `.then()` callback выполняет `populate_worlds_from_level` + `switch_to_world`
- Пока загружается — UI показывает loading overlay (полупрозрачный, "Loading level...")
- Editor camera и viewport замораживаются до завершения

Состояния:
```
m_loading_state: enum { idle, loading_level, importing_model, exporting }
```

**Файлы:**
- `Editor/code/editor_system/edt_editor_system.h/cpp` — `load_level()`, loading state
- `Editor/code/editor_system/edt_editor_layer.cpp` — loading overlay UI

### US-24-5: Заменить require_sync на require + then в editor hot paths
**Status:** done
**Priority:** low

Постепенная замена оставшихся `require_sync` вызовов:

| Место | Текущий | Замена |
|-------|---------|--------|
| Asset drop | `require_sync<desc_base>(tag)` | `require` + `then` (добавить в сцену по callback) |
| `new_level()` | `require_sync<level_desc>` | `require` + `then` |
| `create_world()` | `require_sync<world_desc>` | `require` + `then` |
| Template load | `require_sync` в `load_desc_template` | Кешировать шаблоны при init |

**Файлы:**
- `Editor/code/editor_system/edt_editor_system.cpp` — множественные callsites

### US-24-6: Общий async task runner для editor
**Status:** skipped (overengineering — each async operation has unique completion logic; polling is 2-3 lines)
**Priority:** low

Если US-24-2, US-24-3, US-24-4 показывают повторяющийся паттерн (future + polling + progress UI),
вынести в общую абстракцию:

```cpp
class editor_task {
    virtual void execute() = 0;          // runs on background thread
    virtual float get_progress() const;  // 0..1
    virtual std::string get_status();    // "Parsing geometry..."
    bool is_complete() const;
};

class editor_task_runner {
    void submit(std::unique_ptr<editor_task> task);
    void tick();  // called each frame, dispatches completed tasks to main thread
    editor_task* active_task() const;
};
```

Виджет прогресса — единая modal/overlay панель для всех фоновых операций.

**Файлы:**
- `Editor/code/editor_system/edt_editor_task.h` (новый)
- `Editor/code/editor_system/edt_editor_system.h` — owner task_runner

## Dependency Graph

```
US-24-1 (remove store from serialize_and_push)  ← critical, standalone
    │
    ├── US-24-2 (async import)     ← можно делать параллельно
    ├── US-24-3 (async export)     ← можно делать параллельно
    └── US-24-4 (async level load) ← можно делать параллельно
            │
            └── US-24-5 (replace require_sync) ← после 24-4
                    │
                    └── US-24-6 (task runner) ← если паттерн повторяется
```

## Key Files

| File | Role |
|------|------|
| `Editor/code/editor_system/edt_editor_system.cpp` | serialize_and_push, load_level, asset drop |
| `Editor/code/editor_system/edt_model_importer.h/cpp` | Synchronous Assimp import |
| `Editor/code/editor_system/edt_asset_exporter.h/cpp` | Synchronous export pipeline |
| `Editor/code/editor_system/edt_asset_export_dialog.h/cpp` | Export UI |
| `core/core/resource/res_system.h` | require / require_sync / warmup API |
| `core/engine/scene/level/scn_level.cpp` | reload_world |
| `core/engine/scene/scn_ecs_assembler.cpp` | spawn_from_desc |

## Migration Strategy

1. **US-24-1 первый** — разрывает цикл reload, минимальный diff, сразу стабилизирует редактор
2. **US-24-2 следующий** — самый заметный для UX (импорт моделей — самая долгая операция)
3. **US-24-3 и US-24-4** — параллельно, по приоритету
4. **US-24-5** — постепенно, при касании файлов
5. **US-24-6** — только если паттерн повторяется 3+ раз

## Risks

- **Thread safety**: `active_world_desc()`, `m_world_descs` — не thread-safe. Фоновые задачи
  не должны трогать desc напрямую; результат передаётся через main-thread callback.
- **EnTT registry**: single-threaded. Assembly и ECS-операции — только в main thread.
- **ImGui**: single-threaded. UI updates — только в main thread.
- **Resource cache**: `res_system` имеет mutex на кеше — thread-safe для require/store.
