# EPIC-44: Memory Profiling & Standard Library — трекинг памяти и движковая стандартная библиотека

**Status:** planned
**Theme:** infrastructure
**Dependencies:** EPIC-43 (Memory Allocators)

---

## Мотивация

EPIC-43 дал движку кастомные аллокаторы и per-frame allocator. Но:

1. **Нет видимости по памяти** — невозможно узнать, какой модуль/файл/строка потребляет больше всего памяти. `debug_resource` работает только в Debug и только если его явно подключить.

2. **Неудобно использовать** — `std::pmr::vector<T>(ds::frame_allocator())` verbose, легко забыть передать аллокатор.

3. **Нет визуализации** — профайлер CPU/GPU есть (EPIC-39), а по памяти ничего.

4. **Сырые `new`/`delete`** — вне контроля, нет трекинга.

### Цели

- Автоматический трекинг аллокаций по source_location (файл, строка) без ручной разметки
- Движковая стандартная библиотека контейнеров (`ds::vector`, `ds::string`, ...) с zero-effort трекингом
- Memory overlay в реальном времени с breakdown по файлам/модулям
- Перехват глобальных `new`/`delete` для catch-all статистики

## Архитектурное решение

### Source Location Tracking

`std::source_location::current()` как default parameter захватывает callsite. Ключевое ограничение: работает только на **прямом** call site. Поэтому нужны wrapper-классы, а не alias'ы.

```cpp
// source_location в конструкторе wrapper'а — захватывает строку объявления:
ds::vector<int> v;           // → captures rnd_opaque_pass.cpp:33
ds::vector<int> v2(100, 0);  // → captures rnd_opaque_pass.cpp:34
```

### Allocation Registry

```cpp
namespace ds {
    // Lightweight per-callsite stats (singleton per file:line)
    struct alloc_site_stats {
        const char* file;
        uint32_t line;
        std::atomic<int64_t> current_bytes{0};
        std::atomic<int64_t> peak_bytes{0};
        std::atomic<uint32_t> alloc_count{0};
    };

    // Resolve source_location → persistent alloc_site_stats*
    // Uses a global registry, O(1) lookup after first call (cached)
    alloc_site_stats* resolve_alloc_site(std::source_location loc);

    // Snapshot all sites for overlay
    std::vector<const alloc_site_stats*> alloc_sites_snapshot();
}
```

Категория (render, scene, editor...) выводится автоматически из пути файла:
- `core/engine/render/...` → render
- `core/engine/scene/...` → scene
- `Editor/...` → editor
- fallback → general

### Tracked Memory Resource

```cpp
namespace ds {
    // memory_resource привязанный к callsite. Always-on atomic counters.
    // В Debug: оборачивает upstream в debug_resource (guard bytes, leak tracking).
    class site_resource : public std::pmr::memory_resource {
    public:
        site_resource(alloc_site_stats* site,
                      std::pmr::memory_resource* upstream = std::pmr::get_default_resource());
    };

    // Resolve source_location → persistent site_resource*
    std::pmr::memory_resource* get_tracked_resource(
        std::source_location loc = std::source_location::current());
}
```

### Wrapper Containers (движковая stdlib)

Wrapper-классы наследуют STL/pmr контейнеры, добавляя source_location в конструкторы:

```cpp
namespace ds {
    template<typename T>
    class vector : public std::pmr::vector<T> {
        using base = std::pmr::vector<T>;
    public:
        vector(std::source_location loc = std::source_location::current())
            : base(get_tracked_resource(loc)) {}

        vector(size_t count, const T& value,
               std::source_location loc = std::source_location::current())
            : base(count, value, get_tracked_resource(loc)) {}

        vector(std::initializer_list<T> init,
               std::source_location loc = std::source_location::current())
            : base(init, get_tracked_resource(loc)) {}

        // Конструктор с явным resource (для frame_allocator и т.д.)
        explicit vector(std::pmr::memory_resource* res) : base(res) {}

        // ... forward остальные конструкторы
    };

    // Аналогично:
    // ds::string
    // ds::unordered_map<K, V>
    // ds::deque<T>
}
```

### Frame-scoped фабрики

```cpp
namespace ds {
    // Фабрики для per-frame данных (не трекаются per-site, идут через frame_allocator)
    template<typename T>
    vector<T> make_frame_vector(std::source_location loc = std::source_location::current()) {
        return vector<T>(frame_allocator());
    }

    template<typename T>
    vector<T> make_frame_vector(size_t reserve_count,
                                std::source_location loc = std::source_location::current()) {
        vector<T> v(frame_allocator());
        v.reserve(reserve_count);
        return v;
    }
}
```

### Heap Object Factories

```cpp
namespace ds {
    // make_unique/make_shared с source_location tracking
    template<typename T>
    struct make_unique_at {
        std::source_location loc;
        make_unique_at(std::source_location l = std::source_location::current()) : loc(l) {}

        template<typename... Args>
        std::unique_ptr<T> operator()(Args&&... args) const {
            track_alloc(sizeof(T), loc);
            return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
        }
    };

    // Использование:
    // auto ptr = ds::make_unique_at<MyClass>{}(arg1, arg2);
}
```

### Global new/delete Override

```cpp
// Catch-all: считает total heap usage без per-callsite info.
// Перехватывает всё что не идёт через ds:: контейнеры.
namespace ds {
    struct global_heap_stats {
        std::atomic<int64_t> current_bytes{0};
        std::atomic<int64_t> peak_bytes{0};
        std::atomic<uint64_t> total_allocs{0};
        std::atomic<uint64_t> total_frees{0};
    };
    const global_heap_stats& get_global_heap_stats();
}

// В одном .cpp:
void* operator new(std::size_t size);
void operator delete(void* ptr, std::size_t size) noexcept;
```

### Memory Overlay (ImGui)

Два режима отображения:

**По категориям (из пути файла):**
```
Memory Usage:
  render:    12.4 MB (peak 15.1 MB)  1,240 allocs
  scene:      3.2 MB (peak  3.8 MB)    85 allocs
  resource:  45.6 MB (peak 45.6 MB)   320 allocs
  editor:     1.1 MB (peak  2.3 MB)   412 allocs
  frame:      0.8 MB /  2.0 MB (40%)  per-frame
  heap(raw):  2.3 MB                   catch-all
  TOTAL: 65.4 MB
```

**По файлам (детальный):**
```
Top allocations by file:
  scn_render_data_extractor.cpp:17   8.3 MB   79 allocs
  rnd_opaque_pass.cpp:33             2.1 MB   158 allocs
  res_cache.cpp:45                   1.8 MB   12 allocs
  ...
```

## User Stories

### US-44-1: Allocation registry и site_resource
**Файлы:** `core/core/common/mem_site_resource.h`, `core/core/common/mem_site_resource.cpp`

Реализовать `alloc_site_stats`, registry, `site_resource`, `get_tracked_resource()`.

**AC:**
- [ ] `alloc_site_stats` с atomic counters: current_bytes, peak_bytes, alloc_count
- [ ] `resolve_alloc_site(source_location)` — O(1) lookup после первого вызова
- [ ] `site_resource` наследует `std::pmr::memory_resource`, обновляет counters при alloc/dealloc
- [ ] В Debug: оборачивает upstream в `debug_resource`
- [ ] `alloc_sites_snapshot()` — список всех зарегистрированных sites
- [ ] Категория автоматически определяется из file path
- [ ] Unit-тесты: counters корректны, peak отслеживается, snapshot работает

### US-44-2: Wrapper containers (ds::vector, ds::string, ...)
**Файлы:** `core/core/common/mem_vector.h`, `core/core/common/mem_string.h`, `core/core/common/mem_containers.h`

Wrapper-классы с source_location в конструкторах.

**AC:**
- [ ] `ds::vector<T>` — wrapper над `std::pmr::vector<T>`, source_location в конструкторах
- [ ] `ds::string` — wrapper над `std::pmr::string`
- [ ] Все основные конструкторы std::vector forward'ятся
- [ ] Конструктор с явным `memory_resource*` для frame_allocator и т.д.
- [ ] `ds::make_frame_vector<T>()` — фабрика для per-frame данных
- [ ] Unit-тесты: construction, source_location захватывается корректно, работа с frame_allocator

### US-44-3: Global new/delete override
**Файлы:** `core/core/common/mem_global_new.cpp`

Перехват глобальных `new`/`delete` для catch-all статистики.

**AC:**
- [ ] `operator new` / `operator delete` overridden
- [ ] `ds::get_global_heap_stats()` возвращает current/peak/total counters
- [ ] Sized delete (`operator delete(void*, size_t)`) корректно считает
- [ ] Не ломает third-party библиотеки (assimp, stb, imgui)
- [ ] Unit-тесты: new/delete обновляют counters

### US-44-4: Heap object factories
**Файлы:** `core/core/common/mem_make.h`

`ds::make_unique_at<T>`, `ds::make_shared_at<T>` с source_location.

**AC:**
- [ ] `ds::make_unique_at<T>{}(args...)` — создаёт unique_ptr, трекает alloc site
- [ ] `ds::make_shared_at<T>{}(args...)` — аналогично для shared_ptr
- [ ] Source location захватывается на call site
- [ ] Unit-тесты: объекты создаются, трекинг работает

### US-44-5: Memory profiler overlay (ImGui)
**Файлы:** `core/engine/gui/gui_system/gui_memory_overlay.h`, `core/engine/gui/gui_system/gui_memory_overlay.cpp`
**Зависимости:** US-44-1

ImGui панель с breakdown по категориям и файлам.

**AC:**
- [ ] Панель "Memory" в debug menu
- [ ] Tab "Categories": таблица Category | Current | Peak | Allocs с progress bars
- [ ] Tab "Files": top-N аллокаций по файлу:строке, сортировка по current_bytes
- [ ] Frame allocator: отдельная строка с used/capacity
- [ ] Global heap: отдельная строка catch-all
- [ ] Total row
- [ ] Данные обновляются с настраиваемым интервалом

### US-44-6: Миграция на ds:: контейнеры
**Файлы:** render pipeline, scene, extractors
**Зависимости:** US-44-1, US-44-2

Заменить `std::pmr::vector<T>(ds::frame_allocator())` на `ds::vector<T>` / `ds::make_frame_vector<T>()`.

**AC:**
- [ ] Render pipeline использует `ds::vector` / `ds::make_frame_vector`
- [ ] `rnd_render_packet.hpp` struct members используют `ds::vector`
- [ ] Memory overlay показывает ненулевые значения по категориям и файлам
- [ ] Нет регрессии производительности (профайлер подтверждает)

---

## Порядок выполнения

```
US-44-1 (Registry + site_resource)
  ├── US-44-2 (Wrapper containers) — после US-44-1
  │     └── US-44-6 (Миграция) — после US-44-2
  ├── US-44-3 (Global new/delete) — параллельно
  ├── US-44-4 (Heap factories) — после US-44-1
  └── US-44-5 (Memory overlay) — после US-44-1
```

## Риски

- **Atomic overhead** — ~10ns на аллокацию. Для долгоживущих объектов незначительно. Frame allocator (linear_resource) не проходит через tracked — он отслеживается отдельно (used/capacity).
- **Global new/delete** — может конфликтовать с sanitizers (ASan, MSan). Митигация: `#ifndef` guard, отключать при сборке с sanitizer.
- **STL наследование** — `ds::vector` наследует `std::pmr::vector`. Non-virtual деструктор. Митигация: внутренний тип, не удаляется через base pointer.
- **Registry contention** — `resolve_alloc_site` вызывается при первом создании контейнера. Митигация: per-callsite кеш через static local, lock-free после init.
- **Source location в header** — если контейнер создаётся в inline/template функции, source_location будет на header файл. Митигация: для таких случаев передавать resource явно.

## Критерии завершения эпика

- [ ] `ds::vector<T> v;` автоматически трекает аллокации по source_location
- [ ] Memory overlay показывает breakdown по категориям и файлам
- [ ] Global new/delete catch-all считает total heap usage
- [ ] Overhead трекинга < 1% от frame time
- [ ] Render pipeline мигрирован на ds:: контейнеры
