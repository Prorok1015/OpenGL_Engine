# EPIC-23 — Logger Migration: spdlog Integration

**Status:** done
**Theme:** Infrastructure
**Priority:** medium

## Motivation

Текущий логгер (`engine_log.h/cpp`) — минимальная обёртка над `std::cout`:
- Полностью отключён в Release (`#ifndef _DEBUG`)
- Нет уровней логирования (всё через один `egLOG`)
- Нет файлового вывода, ротации, цветного терминала
- Нет фильтрации по категориям в рантайме
- Категория (`"resource/adapter"`, `"edt/import"`) — просто строка, не используется для маршрутизации

spdlog уже подключён как git-submodule (`lib3dparty/spdlog/`), но не используется.

## Goals

1. Заменить бэкенд логгера на spdlog, сохранив макро-интерфейс `egLOG`
2. Добавить уровни: trace, debug, info, warning, error
3. Обеспечить логирование и в Release-сборках (info+)
4. Обеспечить фильтрацию по категориям
5. Добавить вывод в файл + консоль (multi-sink)

## Non-Goals

- Полная замена всех `egLOG` вызовов на разные уровни (можно постепенно)
- Асинхронный логгер (можно добавить позже)
- Structured logging / JSON output

## Current State

```
core/core/common/logger/
├── engine_log.h      # spdlog macros: egLOG, egLOG_TRACE/DEBUG/INFO/WARN/ERROR
├── engine_log.cpp    # spdlog backend: multi-sink (console + rotating file)
└── CMakeLists.txt    # engine_logger target (links spdlog::spdlog_header_only)
```

`Editor/main.cpp` calls `engine::init_logger()` at startup, `engine::shutdown_logger()` at exit.

## User Stories

### US-23-1: CMake — подключить spdlog к engine_logger
**Status:** done

- `add_subdirectory(lib3dparty/spdlog)` или `find_package` в корневом CMake
- `target_link_libraries(engine_logger PUBLIC spdlog::spdlog_header_only)` (header-only mode)
- Убедиться, что сборка проходит без ошибок

### US-23-2: Бэкенд — заменить std::cout на spdlog
**Status:** done

- Создать spdlog-логгер при первом вызове (lazy init, или explicit `init_logger()`)
- Multi-sink: stdout_color_sink + rotating_file_sink (опционально)
- Категория из первого аргумента `egLOG` → spdlog logger name или prefix
- Формат: `[HH:MM:SS.ms] [category] [level] message`

### US-23-3: Уровни логирования
**Status:** done

- Новые макросы: `egLOG_TRACE`, `egLOG_DEBUG`, `egLOG_INFO`, `egLOG_WARN`, `egLOG_ERROR`
- `egLOG` остаётся как alias для `egLOG_INFO` (обратная совместимость)
- В Debug: default level = trace
- В Release: default level = info
- Compile-time отсечение trace/debug в Release через `SPDLOG_ACTIVE_LEVEL`

### US-23-4: Runtime-фильтрация по категориям
**Status:** done

- Каждая категория (`"resource"`, `"edt"`, `"desc"`) — отдельный spdlog logger
- `engine::set_category_level(category, level)` — per-category override
- `engine::set_log_level(level)` — global level
- CFG_VAR интеграция может быть добавлена на уровне `core_module` в будущем (logger на уровне common, не может зависеть от config)

### US-23-5: Console panel — интеграция с editor
**Status:** done

- `engine::set_log_callback(fn)` — generic callback, вызывается из `log_message()`
- Editor подключает callback в `editor_system::init()`, маппит `engine::log_level` → `edt::log_level`
- Использует `weak_ptr<console_panel>` для безопасного доступа

## Key Files

| File | Role |
|------|------|
| `core/core/common/logger/engine_log.h` | Макросы и API |
| `core/core/common/logger/engine_log.cpp` | Реализация (сейчас std::cout) |
| `core/core/common/logger/CMakeLists.txt` | CMake target |
| `lib3dparty/spdlog/` | spdlog submodule |
| `Editor/code/editor_system/panels/edt_console_panel.h` | Editor console sink target |

## Migration Strategy

1. US-23-1 + US-23-2: подменить бэкенд, сохранив `egLOG` — zero diff в клиентском коде
2. US-23-3: добавить новые макросы, постепенно менять вызовы по мере касания файлов
3. US-23-4 + US-23-5: runtime конфигурация и UI-интеграция

## Format Compatibility

spdlog использует `fmt::format` (или `std::format` с `SPDLOG_USE_STD_FORMAT`).
Текущий `egLOG` уже использует `std::vformat` с `{}` placeholder'ами — синтаксис совместим.
Рекомендуется: `#define SPDLOG_USE_STD_FORMAT` чтобы не тащить fmtlib отдельно.
