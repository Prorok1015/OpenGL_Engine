# EPIC-32: External Assert Dialog — отдельный процесс для ассерт-диалога и краш-репортов

**Theme:** infrastructure
**Status:** planned
**Priority:** lowest
**Depends on:** EPIC-31 (Assert System)

---

## Цель

Вынести отображение ассерт-диалога в отдельное приложение (`assert_dialog`), которое работает независимо от основного процесса. Это гарантирует показ диалога даже при полном краше (GPU fault, heap corruption, stack overflow). Мультиплатформенная реализация: Windows, Linux.

---

## Мотивация

ImGui-диалог из EPIC-31 работает только если основной процесс в состоянии рисовать UI. Не работает когда:
- OpenGL context повреждён (ассерт в render pipeline)
- Heap corruption (std::string / std::format крашатся)
- Stack overflow (нет стека для обработчика)
- Deadlock в главном потоке (ассерт из фонового потока)
- Процесс уже в unhandled exception handler

Внешний процесс не зависит от состояния движка.

---

## Архитектура

```
┌──────────────┐         named pipe / Unix socket         ┌──────────────────┐
│   Editor     │ ──────── assert data (JSON) ──────────▶ │  assert_dialog   │
│  (parent)    │ ◀─────── user action ────────────────── │  (child process) │
│              │                                          │                  │
│  На ассерте: │                                          │  Нативный UI:    │
│  1. Пишет    │                                          │  - Win32 window  │
│     данные   │                                          │  - GTK/Qt dialog │
│  2. Ждёт     │                                          │  - или terminal  │
│     ответа   │                                          │                  │
└──────────────┘                                          └──────────────────┘
```

### Протокол (JSON через pipe)

**Editor → Dialog (request):**
```json
{
  "type": "assert",
  "pid": 12345,
  "condition": "ptr != nullptr",
  "message": "Expected non-null for entity Player",
  "file": "edt_editor_system.cpp",
  "line": 456,
  "function": "create_entity()",
  "stacktrace": "...",
  "log_tail": ["last 50 log lines..."],
  "minidump_path": "crashes/20260317_143022.dmp"
}
```

**Dialog → Editor (response):**
```json
{
  "action": "ignore" | "ignore_all" | "debug" | "abort"
}
```

### Жизненный цикл

**Вариант A — Lazy launch (проще):**
1. Editor стартует без watchdog
2. При первом ассерте: `CreateProcess("assert_dialog")` / `fork+exec`
3. Передаёт данные через stdin/stdout или temp file
4. Ждёт ответа, применяет action

**Вариант B — Watchdog (надёжнее):**
1. Editor при старте запускает `assert_dialog --watch --pipe=\\.\pipe\editor_assert`
2. Dialog слушает pipe в фоне
3. При ассерте editor пишет в pipe, блокируется
4. Dialog показывает окно, шлёт ответ
5. При краше editor'а без ассерта — dialog детектирует broken pipe, предлагает "Send Report"

---

## Мультиплатформенность

| Компонент | Windows | Linux |
|---|---|---|
| IPC | Named pipe (`\\.\pipe\...`) | Unix domain socket |
| UI диалог | Win32 API (`CreateWindowEx`, `MessageLoop`) | GTK3 (если есть) → Zenity fallback → terminal stdin |
| Debug attach | `DebugActiveProcess(pid)` | `ptrace(PTRACE_ATTACH, pid)` или запуск `gdb -p pid` |
| Minidump | `MiniDumpWriteDump()` | core dump (`/proc/pid/coredump_filter`) |
| Process launch | `CreateProcess()` | `fork()` + `execvp()` |

### UI fallback chain

```
Windows:  Win32 native dialog (всегда доступен)
Linux:    GTK3 → Zenity/kdialog → terminal prompt
```

Нативный Win32 диалог не требует OpenGL, DirectX, или каких-либо runtime — работает на любой Windows.

---

## User Stories

### US-32-1 — Приложение assert_dialog (CMake target + минимальный UI)

**Файлы:**
- Новый `tools/assert_dialog/CMakeLists.txt`
- Новый `tools/assert_dialog/main.cpp`
- Новый `tools/assert_dialog/platform_win32.cpp` (Windows UI)
- Новый `tools/assert_dialog/platform_linux.cpp` (Linux UI)
- Новый `tools/assert_dialog/platform.h` (абстракция)

**Результат:** отдельный executable `assert_dialog`, принимает JSON через stdin, показывает нативный диалог, возвращает action через stdout.

---

### US-32-2 — IPC: named pipe (Windows) / Unix socket (Linux)

**Файлы:**
- Новый `tools/assert_dialog/ipc_server.cpp/h` (dialog side)
- Новый `core/core/common/engine_assert_ipc.cpp/h` (editor side)

**Протокол:** JSON-line через pipe. Editor блокируется на read после write. Dialog отвечает после выбора пользователя.

---

### US-32-3 — Интеграция с engine_assert.cpp

**Файлы:**
- `core/core/common/engine_assert.cpp` — при ассерте: попытка отправить в external dialog, fallback на ImGui (EPIC-31), fallback на stderr

**Цепочка fallback:**
1. External process (pipe) → если подключен
2. ImGui dialog (EPIC-31) → если GUI жив
3. stderr + MessageBox → если ничего не работает

---

### US-32-4 — Minidump при краше

**Файлы:**
- Новый `core/core/common/engine_crash_handler.cpp/h`
- `tools/assert_dialog/main.cpp` — отображение информации о краше

**Windows:** `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` → запуск assert_dialog с путём к дампу.
**Linux:** signal handler (`SIGSEGV`, `SIGABRT`) → запуск assert_dialog.

---

### US-32-5 — "Send Report" (заглушка)

**Файлы:**
- `tools/assert_dialog/report_sender.cpp/h`

Кнопка "Send Report" собирает ZIP: minidump + последние 1000 строк лога + system info. Пока сохраняет локально в `crashes/`. HTTP-отправка — placeholder для будущей серверной инфраструктуры.

---

## Новые файлы

```
tools/assert_dialog/
├── CMakeLists.txt
├── main.cpp                    # точка входа, парсинг аргументов
├── platform.h                  # абстракция UI
├── platform_win32.cpp          # Win32 CreateWindowEx диалог
├── platform_linux.cpp          # GTK / Zenity / terminal fallback
├── ipc_server.cpp/h            # слушатель pipe/socket
└── report_sender.cpp/h         # сбор и сохранение краш-репорта

core/core/common/
├── engine_assert_ipc.cpp/h     # клиент: отправка данных в external dialog
└── engine_crash_handler.cpp/h  # unhandled exception → minidump + launch dialog
```

---

## Граф зависимостей

```
EPIC-31 (Assert System — базовый)
  └── EPIC-32 (External Assert Dialog)
        US-32-1 (assert_dialog app)
          ├── US-32-2 (IPC pipe/socket)
          │     └── US-32-3 (интеграция с engine_assert)
          └── US-32-4 (minidump)
                └── US-32-5 (send report заглушка)
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Приложение** | US-32-1 | `assert_dialog` executable с нативным UI на обеих платформах |
| **2 — Связь** | US-32-2, US-32-3 | Editor общается с dialog через pipe; fallback chain работает |
| **3 — Краш-репорт** | US-32-4, US-32-5 | Minidump при unhandled exception; локальное сохранение отчёта |

---

## Риски

| Проблема | Решение |
|---|---|
| GTK недоступен на минимальном Linux | Fallback: Zenity → kdialog → terminal stdin |
| Editor крашится до создания pipe | Lazy launch (вариант A): запуск при первом ассерте; crash handler запускает dialog напрямую |
| Deadlock: editor ждёт ответа, dialog не запустился | Timeout (5 сек) на pipe read; при timeout → fallback на abort |
| Minidump большой (>100MB) | `MiniDumpWithDataSegs` (маленький), не `MiniDumpWithFullMemory` |
| Кросс-компиляция assert_dialog | Отдельный CMake target, минимальные зависимости (только OS API + JSON parser) |

---

## Критерии готовности

- [ ] `assert_dialog` компилируется на Windows и Linux как отдельный executable
- [ ] Нативный диалог отображается без OpenGL/ImGui
- [ ] Editor отправляет ассерт-данные через pipe, получает ответ
- [ ] Fallback chain: external → ImGui → stderr работает корректно
- [ ] Minidump создаётся при unhandled exception
- [ ] "Send Report" сохраняет ZIP с дампом и логами локально
- [ ] Все 4 кнопки (Ignore / Ignore All / Debug / Abort) работают через IPC
