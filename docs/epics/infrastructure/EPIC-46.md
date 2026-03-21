# EPIC-46: Tracy Profiler Integration — внешний профайлер для детального анализа

**Status:** planned
**Theme:** infrastructure
**Dependencies:** EPIC-39 (Performance Profiling Infrastructure)

---

## Мотивация

Внутренний профайлер (EPIC-39) покрывает базовые потребности: `PROFILE_SCOPE`, ImGui overlay, ring buffer. Но для глубокого анализа производительности (наносекундная точность, timeline с иерархией зон, GPU profiling через Tracy GPU zones, memory tracking, сетевое подключение) нужен внешний инструмент.

**Tracy** — лучший выбор для игровых движков на C++:
- Наносекундная точность, минимальный overhead (~2 нс на зону)
- Визуализация timeline с иерархией зон (CPU + GPU)
- Поддержка frame marks, plots, memory tracking
- Header-only клиент, отдельный GUI-сервер для просмотра
- Активно развивается, используется в Godot, Unreal, множестве AAA-студий
- Лицензия: 3-clause BSD

Макро-API (`PROFILE_SCOPE`, `PROFILE_FUNCTION`, `PROFILE_FRAME`) уже спроектировано с учётом Tracy — при `PROFILE_TRACY` макросы должны пробрасывать в `ZoneScoped`, `FrameMark` и т.д.

## User Stories

### US-46-1: Tracy submodule и CMake интеграция
**Файлы:** `lib3dparty/tracy/` (submodule), `core/CMakeLists.txt`

Добавить Tracy как git submodule. Настроить CMake: при `ENGINE_PROFILE_MODE=TRACY` линковать `TracyClient`.

**AC:**
- [ ] Tracy добавлен как submodule в `lib3dparty/tracy`
- [ ] CMake option `ENGINE_PROFILE_MODE=TRACY` линкует `TracyClient`
- [ ] Сборка проходит с `PROFILE_TRACY` без ошибок
- [ ] Сборка без Tracy (NONE, INTERNAL) не затронута

### US-46-2: Макро-проброс в Tracy API
**Файлы:** `core/core/common/eng_profiler.h`
**Зависимости:** US-46-1

При `PROFILE_TRACY` макросы пробрасывают в Tracy:

**AC:**
- [ ] `PROFILE_SCOPE` → `ZoneScoped` / `ZoneScopedN`
- [ ] `PROFILE_FUNCTION` → `ZoneScoped` (Tracy автоматически берёт имя функции)
- [ ] `PROFILE_FRAME` → `FrameMark`
- [ ] `PROFILE_INTERNAL` и `PROFILE_TRACY` взаимоисключающие (compile error при обоих)

### US-46-3: Верификация с Tracy GUI
**Зависимости:** US-46-2

Запустить Editor с `PROFILE_TRACY`, подключиться Tracy GUI, убедиться что данные видны.

**AC:**
- [ ] Данные видны в Tracy GUI при подключении к запущенному Editor
- [ ] Timeline показывает иерархию зон (frame → render → passes)
- [ ] Frame marks корректно разделяют кадры

### US-46-4: GPU Profiling через Tracy
**Файлы:** `core/core/common/eng_profiler.h`, `core/engine/render/render_system/rnd_render_pipeline.cpp`
**Зависимости:** US-46-2

При Tracy — `PROFILE_GPU_SCOPE` пробрасывает в `TracyGpuZone`.

**AC:**
- [ ] При Tracy — GPU-зоны видны на GPU timeline
- [ ] При отсутствии Tracy — существующий OpenGL timer query fallback работает

---

## Порядок выполнения

```
US-46-1 (Submodule + CMake)
  └── US-46-2 (Макро-проброс)
        ├── US-46-3 (Верификация)
        └── US-46-4 (GPU через Tracy)
```

## Риски

- **Tracy submodule размер** — Tracy client header-only (~200KB), сервер не включается в сборку. Минимальный impact.
- **Совместимость компиляторов** — Tracy поддерживает GCC, Clang, MSVC. Проверить на текущем тулчейне.
- **Конфликт с internal profiler** — макросы взаимоисключающие, конфликта быть не должно.

## Критерии завершения эпика

- [ ] Tracy подключается и показывает timeline при `PROFILE_TRACY`
- [ ] GPU-зоны видны на GPU timeline через Tracy
- [ ] Сборка без Tracy не затронута
