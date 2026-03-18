# EPIC-31: Assert System — информативные ассерты с диалогом и стек-трейсом

**Theme:** infrastructure
**Status:** planned
**Depends on:** —

---

## Цель

Заменить текущую минимальную систему ассертов (`assert((msg, cond))`) на полноценную: с человекочитаемым сообщением, стек-трейсом, контекстом (файл/строка/функция), окном диалога (Ignore / Debug / Abort) и интеграцией с логгером.

---

## Проблемы текущей реализации

### 1. Сообщение игнорируется

```cpp
// engine_assert.h
#define ASSERT_MSG(cond, msg, ...) assert((msg, cond))
```

Используется **comma operator** — `msg` вычисляется, но результат отбрасывается. `assert()` проверяет только `cond`. В стандартном окне ассерта на Windows видно лишь `(msg, cond)` как текст выражения — **нечитаемо**.

### 2. Нет стек-трейса

При срабатывании ассерта программа вызывает `abort()`. Разработчик видит только строку ассерта — без call stack, без контекста откуда пришёл вызов.

### 3. Нет выбора действия

Стандартный `assert()` на Windows даёт "Abort / Retry / Ignore", но:
- **Retry** ставит брейкпоинт в CRT, не в месте ассерта
- На Linux/WSL — просто `abort()`, без интерактивности
- Нет опции "Ignore All" (подавить повторные срабатывания того же ассерта)

### 4. В Release — полностью отключены

```cpp
#ifndef _DEBUG
#define ASSERT_MSG(cond, msg)   // пусто
#define ASSERT_FAIL(msg)        // пусто
```

Нет даже логирования. Критические нарушения инвариантов молча проглатываются.

### 5. Нет интеграции с логгером

Ассерты не пишут в `engine_log`. При краше в длительной сессии — нет следа в логах.

### 6. Variadic args не используются

`ASSERT_MSG(cond, msg, ...)` принимает `...` но никогда не форматирует. Нельзя написать `ASSERT_MSG(ptr, "Expected non-null for entity {}", name)`.

---

## Текущее состояние

| Аспект | Состояние |
|---|---|
| Файлы | `engine_assert.h` (9 строк), `engine_assert.cpp` (1 строка) |
| Макросы | `ASSERT_MSG(cond, msg)`, `ASSERT_FAIL(msg)` |
| Debug | `assert((msg, cond))` — стандартный CRT |
| Release | Пусто (no-op) |
| Использование | 55 вызовов в 20 файлах |
| UI диалог | Нет (стандартный Windows CRT dialog) |
| Стек-трейс | Нет |
| Логирование | Нет |

---

## Целевая архитектура

### Новые макросы

```cpp
// Условный ассерт с форматированным сообщением
#define ASSERT_MSG(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            static bool s_ignore = false; \
            if (!s_ignore) { \
                auto action = ::engine::assert_handler( \
                    #cond, __FILE__, __LINE__, __func__, \
                    std::format(fmt __VA_OPT__(,) __VA_ARGS__)); \
                if (action == ::engine::assert_action::ignore_all) s_ignore = true; \
                if (action == ::engine::assert_action::debug_break) ENGINE_DEBUG_BREAK(); \
                if (action == ::engine::assert_action::abort) std::abort(); \
            } \
        } \
    } while (0)

// Безусловный fail
#define ASSERT_FAIL(fmt, ...) ASSERT_MSG(false, fmt __VA_OPT__(,) __VA_ARGS__)

// Verify — проверка и в Release (не отключается)
#define VERIFY_MSG(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            ::engine::assert_handler( \
                #cond, __FILE__, __LINE__, __func__, \
                std::format(fmt __VA_OPT__(,) __VA_ARGS__)); \
        } \
    } while (0)
```

### assert_handler

```cpp
namespace engine {
    enum class assert_action {
        ignore,      // продолжить выполнение
        ignore_all,  // подавить этот ассерт навсегда
        debug_break, // остановиться в дебаггере
        abort        // завершить программу
    };

    // Вызывается при срабатывании ассерта
    assert_action assert_handler(
        const char* expression,
        const char* file,
        int line,
        const char* function,
        std::string message);

    // Пользовательский обработчик (для перенаправления в тесты или UI)
    using assert_callback_t = std::function<assert_action(
        const char* expression, const char* file, int line,
        const char* function, const std::string& message,
        const std::string& stacktrace)>;

    void set_assert_callback(assert_callback_t callback);
}
```

### ENGINE_DEBUG_BREAK

```cpp
#if defined(_MSC_VER)
    #define ENGINE_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
    #define ENGINE_DEBUG_BREAK() __builtin_trap()
#else
    #define ENGINE_DEBUG_BREAK() std::abort()
#endif
```

### Стек-трейс

C++23 `<stacktrace>` или fallback:

```cpp
std::string capture_stacktrace(int skip_frames = 2);
// На MSVC: CaptureStackBackTrace + SymFromAddr
// На GCC/Clang: backtrace() + backtrace_symbols()
// С++23: std::stacktrace::current()
```

---

## User Stories

### US-31-1 — assert_handler + форматированные сообщения

**Файлы:**
- `core/core/common/engine_assert.h` — новые макросы
- `core/core/common/engine_assert.cpp` — `assert_handler()` реализация

**Что делает `assert_handler()`:**
1. Форматирует сообщение: `[ASSERT] condition failed: "ptr != nullptr" — Expected non-null for entity Player`
2. Добавляет файл/строку/функцию: `at edt_editor_system.cpp:456 in create_entity()`
3. Логирует через `egLOG_ERROR("assert", ...)`
4. Вызывает callback (если установлен) или default handler
5. Default handler: если дебаггер подключен → `debug_break`, иначе → `abort`

**Обратная совместимость:** все 55 существующих `ASSERT_MSG(cond, "message")` без format args — продолжают работать.

**Критерии:**
- `ASSERT_MSG(false, "test {} {}", 1, 2)` — форматирует "test 1 2"
- `ASSERT_MSG(false, "simple message")` — работает без args
- В логах появляется запись при каждом срабатывании

---

### US-31-2 — Стек-трейс при ассерте

**Файлы:**
- Новый `core/core/common/engine_stacktrace.h/.cpp`
- `core/core/common/engine_assert.cpp` — вызов `capture_stacktrace()`

**Реализация:**
- MSVC: `CaptureStackBackTrace()` + `SymFromAddr()` (DbgHelp)
- GCC/Clang: `backtrace()` + `backtrace_symbols()` или `<stacktrace>` если доступен
- Форматирует стек в человекочитаемый вид:

```
Stack trace:
  [0] engine::assert_handler() at engine_assert.cpp:42
  [1] edt::editor_system::create_entity() at edt_editor_system.cpp:456
  [2] <lambda>::operator()() at edt_editor_system.cpp:148
  [3] gui::menu_layout_manager::process() at gui_menu_layout.cpp:34
  ...
```

**Критерии:** стек-трейс с именами функций отображается в логе при ассерте.

---

### US-31-3 — Диалог ассерта (ImGui overlay)

**Файлы:**
- Новый `core/core/common/engine_assert_dialog.h/.cpp`
- `core/core/common/engine_assert.cpp` — подключение диалога

**Диалог (ImGui modal):**

```
┌─────────────────────────────────────────────┐
│  ⚠ ASSERTION FAILED                        │
│                                             │
│  Condition:  ptr != nullptr                 │
│  Message:    Expected non-null for entity   │
│              "Player"                       │
│                                             │
│  Location:   edt_editor_system.cpp:456      │
│  Function:   create_entity()                │
│                                             │
│  Stack trace:                               │
│  ┌─────────────────────────────────────┐    │
│  │ [0] engine::assert_handler()       │    │
│  │ [1] edt::editor_system::create_... │    │
│  │ [2] <lambda>::operator()()         │    │
│  │ ...                                │    │
│  └─────────────────────────────────────┘    │
│                                             │
│  [Ignore] [Ignore All] [Debug] [Abort]      │
│  ☐ Copy to clipboard                        │
└─────────────────────────────────────────────┘
```

**Кнопки:**
- **Ignore** — продолжить выполнение (return `ignore`)
- **Ignore All** — подавить этот ассерт (static bool в макросе)
- **Debug** — `__debugbreak()` (остановка в дебаггере)
- **Abort** — `std::abort()` (завершение)
- **Copy to clipboard** — скопировать всю информацию в буфер обмена

**Fallback:** если ImGui недоступен (до инициализации GUI или headless mode) — stderr + default action.

**Критерии:** при ассерте появляется модальное окно поверх всего, выполнение приостановлено до выбора действия.

---

### US-31-4 — Console fallback (без GUI)

**Файлы:**
- `core/core/common/engine_assert.cpp`

Если GUI не инициализирован или assert произошёл не в главном потоке:

```
====================================
ASSERTION FAILED
Condition:  ptr != nullptr
Message:    Expected non-null for entity "Player"
Location:   edt_editor_system.cpp:456
Function:   create_entity()

Stack trace:
  [0] engine::assert_handler()
  [1] edt::editor_system::create_entity()
  ...

Actions: [I]gnore  [A]bort  [D]ebug break
====================================
```

На Windows без консоли — `MessageBoxA()` с текстом ассерта.

**Критерии:** ассерт до `init_logger()` / в фоновом потоке — выводит в stderr или MessageBox.

---

### US-31-5 — VERIFY_MSG для Release builds

**Файлы:**
- `core/core/common/engine_assert.h`

`VERIFY_MSG(cond, fmt, ...)` — аналог `ASSERT_MSG`, но **не отключается в Release**. Используется для критических инвариантов (null resource, invalid state).

```cpp
// Release: проверяет условие, логирует, НЕ прерывает выполнение
// Debug: полный ассерт с диалогом
```

**Миграция:** заменить ключевые `ASSERT_MSG` на `VERIFY_MSG` в:
- `res_system.h` — проверки при загрузке ресурсов
- `ds_store.hpp` — `require()` несуществующего типа
- `ecs_system.h` — отсутствие `runtime_context_provider`

**Критерии:** `VERIFY_MSG` логирует ошибку в Release, показывает диалог в Debug.

---

### US-31-6 — Интеграция с editor console

**Файлы:**
- `Editor/code/editor_system/edt_editor_system.cpp` — `set_assert_callback`

При срабатывании ассерта в editor — отправить запись в console panel:

```cpp
engine::set_assert_callback([console_weak](const char* expr, const char* file, int line,
    const char* func, const std::string& msg, const std::string& stack) -> engine::assert_action
{
    if (auto panel = console_weak.lock()) {
        panel->add_log(edt::log_level::error,
            std::format("[ASSERT] {} — {} ({}:{})", msg, expr, file, line));
    }
    // Show ImGui dialog
    return engine::show_assert_dialog(expr, file, line, func, msg, stack);
});
```

**Критерии:** ассерты видны в console panel как error-записи.

---

## Новые файлы

```
core/core/common/
├── engine_assert.h                 ← переписать макросы (US-31-1)
├── engine_assert.cpp               ← assert_handler() + fallback (US-31-1, US-31-4)
├── engine_assert_dialog.h/.cpp     ← ImGui диалог (US-31-3)
└── engine_stacktrace.h/.cpp        ← capture_stacktrace() (US-31-2)
```

---

## Граф зависимостей

```
US-31-1 (assert_handler + format)  ← первый, standalone
  ├── US-31-2 (стек-трейс)
  │     └── US-31-3 (ImGui диалог)
  ├── US-31-4 (console fallback)
  └── US-31-5 (VERIFY_MSG)

US-31-6 (editor console)  ← после US-31-1 + US-31-3
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Ядро** | US-31-1 | Форматированные сообщения, логирование, `assert_handler` |
| **2 — Диагностика** | US-31-2, US-31-4 | Стек-трейс + console fallback |
| **3 — UI** | US-31-3, US-31-6 | ImGui диалог + записи в editor console |
| **4 — Release** | US-31-5 | `VERIFY_MSG` для критических проверок |

---

## Риски

| Проблема | Решение |
|---|---|
| `<stacktrace>` не поддержан компилятором | Fallback на platform API (`CaptureStackBackTrace` / `backtrace`) |
| Ассерт в render loop → диалог блокирует render | Диалог рисуется в отдельном ImGui pass поверх всего; render loop не зависнет |
| Ассерт в background thread (async export) | Console fallback (US-31-4); thread-safe handler с mutex |
| Дебаг-символы не загружены | Показать hex-адреса вместо имён; предложить "Copy to clipboard" для анализа |
| 55 callsite'ов — обратная совместимость | Новый `ASSERT_MSG` принимает тот же формат; `std::format("literal")` без args — валиден |

---

## Критерии готовности

- [ ] `ASSERT_MSG(cond, "msg {}", arg)` форматирует и логирует сообщение
- [ ] Стек-трейс отображается при ассерте (минимум 5 фреймов с именами функций)
- [ ] ImGui диалог с 4 кнопками (Ignore / Ignore All / Debug / Abort)
- [ ] "Ignore All" подавляет повторные срабатывания конкретного ассерта
- [ ] Console fallback при отсутствии GUI (stderr / MessageBox)
- [ ] `VERIFY_MSG` работает в Release (логирует, не прерывает)
- [ ] Ассерты видны в editor console panel
- [ ] Все 55 существующих вызовов компилируются без изменений
