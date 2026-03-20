# EPIC-39: Performance Profiling Infrastructure — замеры производительности

**Status:** in_progress
**Theme:** infrastructure
**Dependencies:** EPIC-23 (Logger — spdlog), EPIC-37 (Unit Tests — для бенчмарков)

---

## Мотивация

Сейчас в движке нет систематического способа измерить, что и сколько времени занимает. Есть только:
- `eng_performance_timer.hpp` — ручной `scoped_timer`, пишет в `std::cout`
- `gui_debug_layer` — показывает общий FPS через ImGui

Этого недостаточно для:
- Понимания, где узкие места (рендер? ECS update? ресурсная загрузка?)
- Отслеживания регрессий производительности между коммитами
- Принятия обоснованных решений при оптимизации

### Выбор инструмента: Tracy Profiler

**Tracy** — лучший выбор для игровых движков на C++:
- Наносекундная точность, минимальный overhead (~2 нс на зону)
- Визуализация timeline с иерархией зон (CPU + GPU)
- Поддержка frame marks, plots, memory tracking
- Header-only клиент, отдельный GUI-сервер для просмотра
- Активно развивается, используется в Godot, Unreal, множестве AAA-студий
- Лицензия: 3-clause BSD

**Альтернативы и почему не они:**
- **Optick** — хорош, но менее активно поддерживается
- **Chrome Tracing (JSON)** — простой формат, но нет live-просмотра и GPU
- **Custom решение** — зачем писать своё, когда есть Tracy
- **RenderDoc/NSight** — для GPU-отладки, не для CPU-профилирования

### Стратегия интеграции

Макросы-обёртки (`PROFILE_SCOPE`, `PROFILE_FUNCTION`) — чтобы:
1. При `TRACY_ENABLE` — пробрасывать в Tracy
2. Без Tracy — fallback на лёгкий internal profiler (для CI/тестов без GUI)
3. В Release без профайлинга — компилируются в ноль

## Архитектурное решение

### Новые файлы

```
core/core/common/eng_profiler.h       ← макросы PROFILE_SCOPE, PROFILE_FUNCTION, PROFILE_FRAME
core/core/common/eng_profiler.cpp     ← fallback реализация (без Tracy)
```

### Макро-API

```cpp
// Замер именованной зоны
PROFILE_SCOPE("LoadTexture")

// Замер текущей функции (имя = __FUNCTION__)
PROFILE_FUNCTION()

// Отметка конца кадра (для FPS-трекинга)
PROFILE_FRAME("MainThread")

// Именованный plot (числовое значение на графике)
PROFILE_PLOT("EntityCount", registry.size())

// GPU-зона (для OpenGL)
PROFILE_GPU_SCOPE("OpaquePass")
```

### Уровни сборки (CMake option)

| CMake Option | Tracy | Fallback | Overhead |
|---|---|---|---|
| `PROFILE_NONE` (default Release) | OFF | OFF | Zero — макросы пустые |
| `PROFILE_INTERNAL` | OFF | ON | Лёгкий — scoped_timer + ring buffer |
| `PROFILE_TRACY` | ON | OFF | ~2 нс/зону — полный Tracy |

### Fallback Internal Profiler

Для случая без Tracy — минимальный ring buffer последних N замеров:
- Хранит: zone name, duration, thread_id, depth
- Выводится в ImGui overlay (расширение `gui_debug_layer`)
- Дампится в лог по запросу (hotkey или команда)

```cpp
struct profile_entry {
    const char* name;
    float duration_ms;
    uint32_t depth;
    std::thread::id thread_id;
};

// Ring buffer на ~1000 записей, lock-free single-producer
```

## User Stories

### US-39-1: Макро-обёртки и fallback profiler
**Файлы:** `core/core/common/eng_profiler.h`, `core/core/common/eng_profiler.cpp`, `core/CMakeLists.txt`

Создать `eng_profiler.h` с макросами `PROFILE_SCOPE(name)`, `PROFILE_FUNCTION()`, `PROFILE_FRAME(name)`. Реализовать fallback: `scoped_profiler_zone` с ring buffer. Добавить CMake option `ENGINE_PROFILE_MODE` (NONE / INTERNAL / TRACY).

**AC:**
- [x] `PROFILE_SCOPE("Test")` компилируется и замеряет время блока
- [x] `PROFILE_FUNCTION()` использует `__FUNCTION__` как имя
- [x] При `PROFILE_NONE` макросы раскрываются в пустоту (проверить в Compiler Explorer или asm)
- [x] При `PROFILE_INTERNAL` данные попадают в ring buffer
- [x] Ring buffer thread-safe для single-producer (один поток пишет)
- [x] Существующий `eng_performance_timer.hpp` удалён — заменён на `eng_profiler.h`
- [x] Unit-тесты: замер зоны возвращает >0, вложенные зоны корректно считают depth

### US-39-2: Инструментирование main loop и ключевых систем
**Файлы:** `Editor/code/editor_system/core/edt_frame_loop_service.cpp`, `core/engine/render/render_system/*.cpp`, `core/engine/scene/scene_system/*.cpp`
**Зависимости:** US-39-1

Расставить `PROFILE_SCOPE` / `PROFILE_FUNCTION` в ключевых точках:
- Frame loop (начало/конец кадра, `PROFILE_FRAME`)
- ECS system update (каждая система)
- Render passes (opaque, transparent, grid, skybox)
- Resource loading (адаптеры: assimp, stb, text)
- Scene load/save

**AC:**
- [x] `PROFILE_FRAME("MainThread")` вызывается каждый кадр
- [x] Каждый render pass имеет `PROFILE_SCOPE`
- [x] ECS system_factory вызывает `PROFILE_SCOPE` при update каждой системы
- [x] Ресурсные адаптеры (assimp, stb) имеют `PROFILE_SCOPE` на load
- [x] Не менее 15 точек инструментирования в движке

### US-39-3: ImGui Profiler Overlay
**Файлы:** `core/engine/gui/gui_system/gui_debug_layer.h`, `core/engine/gui/gui_system/gui_debug_layer.cpp`
**Зависимости:** US-39-1

Расширить debug layer: окно "Profiler" с таблицей последних замеров и простой timeline-полоской текущего кадра.

**AC:**
- [x] Новый пункт меню "Debug/Profiler" открывает окно
- [x] Таблица: Zone Name | Avg ms | Max ms | Calls/frame
- [x] Цветная полоска кадра: пропорциональные секции по зонам
- [x] Данные обновляются каждый кадр из ring buffer
- [x] Работает при `PROFILE_INTERNAL` без Tracy

### US-39-4: Tracy Integration
**Файлы:** `lib3dparty/tracy/` (submodule), `core/CMakeLists.txt`, `core/core/common/eng_profiler.h`
**Зависимости:** US-39-1

Добавить Tracy как git submodule. При `PROFILE_TRACY` макросы пробрасывают в `ZoneScoped`, `FrameMark` и т.д.

**AC:**
- [ ] Tracy добавлен как submodule в `lib3dparty/tracy`
- [ ] `PROFILE_TRACY` линкует `TracyClient`
- [ ] `PROFILE_SCOPE` → `ZoneScoped` / `ZoneScopedN`
- [ ] `PROFILE_FRAME` → `FrameMark`
- [ ] Данные видны в Tracy GUI при подключении к запущенному Editor
- [ ] `PROFILE_INTERNAL` и `PROFILE_TRACY` не конфликтуют (взаимоисключающие)

### US-39-5: GPU Profiling (OpenGL Timer Queries)
**Файлы:** `core/core/common/eng_profiler.h`, `core/engine/render/render_system/rnd_render_pipeline.cpp`
**Зависимости:** US-39-2, US-39-4

Добавить `PROFILE_GPU_SCOPE` через OpenGL timer queries (`GL_TIME_ELAPSED`). При Tracy — пробрасывать в `TracyGpuZone`.

**AC:**
- [x] `PROFILE_GPU_SCOPE("OpaquePass")` замеряет время на GPU
- [x] Результаты доступны через ring buffer (с задержкой 1-2 кадра — особенность GPU queries)
- [ ] При Tracy — GPU-зоны видны на GPU timeline
- [x] При отсутствии поддержки timer queries — graceful fallback (не крашится)

### US-39-6: Миграция scoped_timer → PROFILE_SCOPE
**Файлы:** все файлы, использующие `eng_performance_timer.hpp`
**Зависимости:** US-39-1

Заменить прямое использование `ds::scoped_timer` (stdout) на `PROFILE_SCOPE`. Удалить `std::cout` из `scoped_timer` или пометить deprecated.

**AC:**
- [x] Нет прямых вызовов `ds::scoped_timer` в production-коде
- [x] `scoped_timer` помечен `[[deprecated]]` или удалён
- [x] Все замеры идут через единый `PROFILE_*` API

---

## Порядок выполнения

```
US-39-1 (Макросы + fallback)
  ├── US-39-2 (Инструментирование)
  │     └── US-39-5 (GPU Profiling)
  ├── US-39-3 (ImGui Overlay)
  └── US-39-6 (Миграция scoped_timer)

US-39-4 (Tracy) — параллельно с US-39-2/39-3, после US-39-1
```

## Риски

- **Tracy submodule размер** — Tracy client header-only (~200KB), сервер не включается в сборку. Минимальный impact.
- **OpenGL timer queries** — на некоторых драйверах неточны. Митигация: проверять `GL_ARB_timer_query`, graceful fallback.
- **Overhead в Debug** — ring buffer + scoped guards. Митигация: PROFILE_NONE по умолчанию в Release.
- **Thread safety** — если добавим многопоточность позже. Митигация: ring buffer per-thread с merge при чтении.

## Критерии завершения эпика

- [ ] `PROFILE_SCOPE` / `PROFILE_FUNCTION` / `PROFILE_FRAME` работают во всех трёх режимах (NONE, INTERNAL, TRACY)
- [ ] Не менее 15 точек инструментирования в движке (loop, render, ECS, resources)
- [ ] ImGui overlay показывает breakdown текущего кадра
- [ ] Tracy подключается и показывает timeline при `PROFILE_TRACY`
- [ ] GPU-замеры работают для render passes
- [ ] Старый `scoped_timer` заменён на `PROFILE_*`
