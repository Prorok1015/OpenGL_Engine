# EPIC-23 — Logger Migration: spdlog Integration

**Status:** planned
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
├── engine_log.h      # egLOG macro, logger() overloads
├── engine_log.cpp    # std::cout implementation
└── CMakeLists.txt    # engine_logger target (no spdlog link)
```

## User Stories

### US-23-1: CMake — подключить spdlog к engine_logger
**Status:** todo

- `add_subdirectory(lib3dparty/spdlog)` или `find_package` в корневом CMake
- `target_link_libraries(engine_logger PUBLIC spdlog::spdlog_header_only)` (header-only mode)
- Убедиться, что сборка проходит без ошибок

### US-23-2: Бэкенд — заменить std::cout на spdlog
**Status:** todo

- Создать spdlog-логгер при первом вызове (lazy init, или explicit `init_logger()`)
- Multi-sink: stdout_color_sink + rotating_file_sink (опционально)
- Категория из первого аргумента `egLOG` → spdlog logger name или prefix
- Формат: `[HH:MM:SS.ms] [category] [level] message`

### US-23-3: Уровни логирования
**Status:** todo

- Новые макросы: `egLOG_TRACE`, `egLOG_DEBUG`, `egLOG_INFO`, `egLOG_WARN`, `egLOG_ERROR`
- `egLOG` остаётся как alias для `egLOG_INFO` (обратная совместимость)
- В Debug: default level = trace
- В Release: default level = info
- Compile-time отсечение trace/debug в Release через `SPDLOG_ACTIVE_LEVEL`

### US-23-4: Runtime-фильтрация по категориям
**Status:** todo

- Каждая категория (`"resource"`, `"edt"`, `"desc"`) — отдельный spdlog logger
- CFG_VAR для глобального уровня: `logger.level = "info"`
- Опционально: CFG_VAR для per-category level: `logger.level.edt = "debug"`

### US-23-5: Console panel — интеграция с editor
**Status:** todo

- Добавить custom spdlog sink, который пишет в `edt::console_panel`
- Console panel уже имеет `add_log(level, msg)` — нужно подключить sink
- Фильтрация по уровням в UI уже есть

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
