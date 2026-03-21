# EPIC-43: Memory Allocators — система управления памятью движка

**Status:** done
**Theme:** infrastructure
**Dependencies:** none

---

## Мотивация

Профайлер (EPIC-39) показал, что основная нагрузка кадра — в экстракции данных сцены и render passes. Значительная часть этого времени уходит на аллокации: `std::vector` grow, временные контейнеры, per-frame буферы данных. Стандартный аллокатор (`new`/`delete`) — универсальный, но медленный для игрового цикла:

- **Per-frame аллокации** — каждый кадр создаются и уничтожаются десятки мелких буферов (render packets, light lists, draw calls). Нужен linear/bump allocator, который сбрасывается за O(1) в начале кадра.
- **Pool аллокации** — ECS компоненты, ноды сцены, ресурсные хендлы — объекты фиксированного размера, создаются/удаляются часто. Pool allocator устраняет фрагментацию и ускоряет alloc/free до O(1).
- **Контейнеры** — `std::vector`, `std::unordered_map` и другие STL контейнеры должны уметь работать с кастомными аллокаторами через `std::pmr` или шаблонный параметр.
- **Отладка** — утечки, double-free, use-after-free должны ловиться в Debug-сборке.

### Стратегия

1. Определить базовый интерфейс аллокатора (`ds::allocator_interface` или `std::pmr::memory_resource`)
2. Реализовать конкретные аллокаторы (linear, pool, stack)
3. Интегрировать с STL через `std::pmr` контейнеры или Allocator-aware шаблоны
4. Заменить hot-path аллокации в render pipeline и экстракторах

### Выбор: `std::pmr` vs custom

**`std::pmr::memory_resource`** — стандартный полиморфный аллокатор C++17:
- Совместим с `std::pmr::vector`, `std::pmr::string`, etc.
- Цепочка fallback (upstream resource)
- Уже есть `monotonic_buffer_resource`, `synchronized_pool_resource`

**Рекомендация:** использовать `std::pmr` как базу. Собственные аллокаторы наследуем от `std::pmr::memory_resource`. Контейнеры на hot path заменяем на `std::pmr::vector<T>` и т.д.

## Архитектурное решение

### Новые файлы

```
core/core/common/mem_linear_allocator.h      ← linear (bump/frame) allocator
core/core/common/mem_linear_allocator.cpp
core/core/common/mem_pool_allocator.h        ← fixed-size pool allocator
core/core/common/mem_pool_allocator.cpp
core/core/common/mem_stack_allocator.h       ← stack (LIFO) allocator
core/core/common/mem_stack_allocator.cpp
core/core/common/mem_debug_resource.h        ← debug wrapper: leak/overrun detection
core/core/common/mem_debug_resource.cpp
core/core/common/mem_allocator.h             ← общий include + per-frame globals
```

### Аллокаторы

| Аллокатор | Наследует | Назначение | alloc | free | reset |
|-----------|-----------|-----------|-------|------|-------|
| `linear_resource` | `std::pmr::memory_resource` | Per-frame данные, временные буферы | O(1) bump | no-op | O(1) сброс указателя |
| `pool_resource` | `std::pmr::memory_resource` | Объекты одного размера (компоненты, ноды) | O(1) free-list | O(1) return to free-list | O(1) сброс |
| `stack_resource` | `std::pmr::memory_resource` | LIFO аллокации (вложенные scope) | O(1) bump | O(1) если LIFO порядок | O(1) до маркера |
| `debug_resource` | `std::pmr::memory_resource` | Обёртка: guard bytes, leak tracking | delegates | delegates | delegates |

### Per-frame allocator

```cpp
// В начале кадра:
ds::frame_allocator().reset();

// Использование:
std::pmr::vector<rnd::render_packet> packets(ds::frame_allocator());
packets.reserve(estimated_count);
// ... заполняем ...
// В конце кадра — память освобождается автоматически при reset()
```

### Интеграция с существующим кодом

```cpp
// До:
std::vector<rnd::draw_call> draw_calls;

// После:
std::pmr::vector<rnd::draw_call> draw_calls(ds::frame_allocator());
```

## User Stories

### US-43-1: Linear (frame) allocator
**Файлы:** `core/core/common/mem_linear_allocator.h`, `core/core/common/mem_linear_allocator.cpp`, `core/CMakeLists.txt`

Реализовать `ds::linear_resource` — наследник `std::pmr::memory_resource`. Аллокация — bump pointer, free — no-op, reset — сброс указателя на начало.

**AC:**
- [x] `linear_resource` наследует `std::pmr::memory_resource`
- [x] Конструктор принимает `void* buffer, size_t size` или `size_t size` (сам аллоцирует через upstream)
- [x] `allocate()` — O(1) bump с выравниванием
- [x] `deallocate()` — no-op
- [x] `reset()` — сброс pointer, O(1)
- [x] При исчерпании буфера — fallback на upstream resource или `std::bad_alloc`
- [x] `std::pmr::vector<int> v(&alloc)` работает корректно
- [x] Unit-тесты: аллокация, выравнивание, reset, overflow

### US-43-2: Pool allocator
**Файлы:** `core/core/common/mem_pool_allocator.h`, `core/core/common/mem_pool_allocator.cpp`

Реализовать `ds::pool_resource` — fixed-size block allocator с free-list.

**AC:**
- [x] Конструктор принимает `size_t block_size, size_t block_count`
- [x] `allocate()` — O(1) из free-list
- [x] `deallocate()` — O(1) return в free-list
- [x] При исчерпании — рост (новый chunk) или fallback на upstream
- [x] `reset()` — возврат всех блоков в free-list
- [x] Unit-тесты: alloc/free циклы, исчерпание, reset

### US-43-3: Stack (LIFO) allocator
**Файлы:** `core/core/common/mem_stack_allocator.h`, `core/core/common/mem_stack_allocator.cpp`

Реализовать `ds::stack_resource` — LIFO аллокатор с маркерами.

**AC:**
- [x] `allocate()` — O(1) bump
- [x] `deallocate()` — O(1) только если LIFO порядок (иначе no-op)
- [x] `marker()` — возвращает текущую позицию
- [x] `reset_to_marker(m)` — сброс до маркера
- [x] Unit-тесты: вложенные scope, маркеры, LIFO free

### US-43-4: Debug memory resource
**Файлы:** `core/core/common/mem_debug_resource.h`, `core/core/common/mem_debug_resource.cpp`

Обёртка над любым `memory_resource`: guard bytes (canary), leak tracking, double-free detection.

**AC:**
- [x] Обёртка: `debug_resource(std::pmr::memory_resource* upstream)`
- [x] Guard bytes до и после каждого блока — обнаружение buffer overrun
- [x] Tracking: `active_allocations` map (pointer → size + callsite)
- [x] `report_leaks()` — лог всех неосвобождённых блоков
- [x] Double-free detection — assert при повторном free
- [x] Активен только при `#ifndef NDEBUG`
- [x] Unit-тесты: leak detection, overrun detection, double-free

### US-43-5: Per-frame allocator global API
**Файлы:** `core/core/common/mem_allocator.h`, frame loop integration

Глобальный per-frame аллокатор: `ds::frame_allocator()` возвращает `std::pmr::memory_resource*`, `ds::frame_allocator_reset()` вызывается в начале кадра.

**AC:**
- [x] `ds::frame_allocator()` — thread-local, возвращает `memory_resource*`
- [x] `ds::frame_allocator_reset()` — сброс линейного аллокатора
- [x] Интегрирован в frame loop (reset в начале кадра)
- [x] Начальный размер буфера конфигурируется через `CFG_VAR`
- [x] Unit-тесты: аллокация через `frame_allocator()`, reset между кадрами

### US-43-6: Замена hot-path аллокаций в render pipeline
**Файлы:** `core/engine/render/render_system/*.cpp`, `core/engine/scene/level/scn_light_extractor.cpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`
**Зависимости:** US-43-1, US-43-5

Заменить `std::vector` на `std::pmr::vector` с `frame_allocator()` в горячих путях: render packets, draw calls, light data, extract results.

**AC:**
- [x] `render_packet` lists используют `frame_allocator`
- [x] Light extractor использует `frame_allocator` для временных буферов
- [x] Render data extractor использует `frame_allocator`
- [x] Render passes не аллоцируют из heap на hot path
- [x] Внутренний профайлер подтверждает работу frame_allocator на hot path

---

## Порядок выполнения

```
US-43-1 (Linear allocator)
  ├── US-43-3 (Stack allocator) — использует похожую механику
  ├── US-43-5 (Per-frame global API) — обёртка над linear
  │     └── US-43-6 (Hot-path замена) — применение в render pipeline
  └── US-43-4 (Debug resource) — обёртка, тестирует любой аллокатор

US-43-2 (Pool allocator) — параллельно с US-43-1
```

## Риски

- **`std::pmr` overhead** — виртуальные вызовы `do_allocate`/`do_deallocate`. Митигация: для критических путей можно использовать шаблонные аллокаторы напрямую, `pmr` — для удобства.
- **Thread safety** — `linear_resource` НЕ thread-safe по дизайну (per-thread). Если понадобится — `synchronized_pool_resource` из стандартной библиотеки.
- **Размер frame buffer** — слишком маленький → частые fallback на upstream. Слишком большой → лишнее потребление. Митигация: `CFG_VAR` + статистика watermark.
- **STL совместимость** — не все контейнеры trivially заменяются на `pmr`. Митигация: начать с `vector`, `string` — самые частые на hot path.

## Критерии завершения эпика

- [x] Три аллокатора (linear, pool, stack) реализованы и покрыты тестами
- [x] Debug resource ловит leaks, overruns, double-free в Debug сборке
- [x] Per-frame allocator интегрирован в frame loop
- [x] Render pipeline и экстракторы используют `frame_allocator` на hot path
- [x] Внутренний профайлер подтверждает работу аллокаторов (детальные бенчмарки — в EPIC-44)
