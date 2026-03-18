# EPIC-40: Icon Font Integration — иконочный шрифт для UI редактора

**Status:** planned
**Theme:** editor
**Dependencies:** none

---

## Мотивация

ImGui по умолчанию рендерит текст через встроенный ProggyClean или системный шрифт без emoji/иконок. UTF-8 emoji (`📋`, `🔍`, `⚙️`) отображаются как `?` или пустые квадраты.

Для профессионального UI редактора нужны иконки: кнопки копирования, папки в asset browser, типы компонентов в inspector, gizmo-кнопки и т.д. Иконочный шрифт (Font Awesome или Material Icons) — стандартное решение для ImGui-приложений.

## Архитектурное решение

### Выбор: Font Awesome 6 Free

- Самый популярный иконочный шрифт для ImGui (~2000 иконок в Free-версии)
- Есть готовый header [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) с `#define ICON_FA_COPY "\xef\x83\x85"`
- Лицензия: SIL OFL (шрифт) + MIT (CSS/header) — совместимо
- Размер: ~400KB (fa-solid-900.ttf)

### Интеграция через MergeMode

```cpp
// В gui_gl_backend.cpp, после AddFontDefault:
ImFontConfig icon_cfg;
icon_cfg.MergeMode = true;
icon_cfg.GlyphMinAdvanceX = 16.0f;
icon_cfg.PixelSnapH = true;
static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
io.Fonts->AddFontFromFileTTF("res/fonts/fa-solid-900.ttf", 14.0f, &icon_cfg, icon_ranges);
```

### Использование

```cpp
#include "IconsFontAwesome6.h"

ImGui::Button(ICON_FA_COPY " Copy");      // кнопка с иконкой
ImGui::Text(ICON_FA_FOLDER " Assets");     // текст с иконкой
ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save");  // меню
```

## User Stories

### US-40-1: Добавить Font Awesome шрифт и header
**Файлы:** `Editor/res/fonts/fa-solid-900.ttf`, `core/engine/gui/gui_system/IconsFontAwesome6.h`

Скачать Font Awesome 6 Free (fa-solid-900.ttf) и header IconFontCppHeaders. Разместить шрифт в `Editor/res/fonts/`, header рядом с gui_system.

**AC:**
- [ ] `fa-solid-900.ttf` лежит в `Editor/res/fonts/`
- [ ] `IconsFontAwesome6.h` доступен для include из gui_system
- [ ] Файлы добавлены в git (не в .gitignore)

### US-40-2: Загрузка иконочного шрифта при инициализации
**Файлы:** `core/engine/gui/backends/opengl/gui_gl_backend.cpp`
**Зависимости:** US-40-1

Добавить загрузку fa-solid-900.ttf через MergeMode после основного шрифта. Путь к шрифту определять через resource system или относительно executable.

**AC:**
- [ ] Иконочный шрифт загружается при старте без ошибок
- [ ] `MergeMode = true` — иконки дополняют основной шрифт, не заменяют
- [ ] DPI-scaling применяется к иконочному шрифту так же как к основному
- [ ] Если файл шрифта не найден — graceful fallback (лог + продолжение без иконок)

### US-40-3: Применить иконки в Profiler Overlay
**Файлы:** `core/engine/gui/gui_system/gui_profiler_overlay.cpp`
**Зависимости:** US-40-2

Заменить текстовые заглушки `[=]` на настоящие иконки Font Awesome.

**AC:**
- [ ] Кнопка Copy использует `ICON_FA_COPY`
- [ ] Кнопка Dump to Log использует `ICON_FA_FILE_LINES` или аналог
- [ ] Иконки отображаются корректно рядом с текстом

### US-40-4: Применить иконки в основных панелях редактора
**Файлы:** `Editor/code/editor_system/panels/*.cpp`, `Editor/code/editor_system/controller/edt_editor_layer.cpp`
**Зависимости:** US-40-2

Добавить иконки в ключевые места UI: меню File/Edit/View, scene hierarchy (сущности, компоненты), asset browser (типы файлов), inspector, toolbar.

**AC:**
- [ ] Меню File: иконки Save, Load, New, Exit
- [ ] Scene Hierarchy: иконка Entity, иконки для разных типов узлов
- [ ] Asset Browser: иконки для папок и типов файлов (.glb, .desc, .png)
- [ ] Inspector: иконки типов компонентов
- [ ] Не менее 15 иконок расставлено по UI

### US-40-5: Документация — гайд по добавлению иконок
**Файлы:** `CLAUDE.md` или `Editor/CLAUDE.md`
**Зависимости:** US-40-2

Добавить секцию в CLAUDE.md о том, как использовать иконки: include, формат использования, ссылка на cheatsheet.

**AC:**
- [ ] В CLAUDE.md описан паттерн использования иконок
- [ ] Указана ссылка на Font Awesome cheatsheet
- [ ] Пример кода с ICON_FA_* макросом

---

## Порядок выполнения

```
US-40-1 (Файлы шрифта + header)
  └── US-40-2 (Загрузка в backend)
        ├── US-40-3 (Иконки в Profiler)
        ├── US-40-4 (Иконки в панелях)
        └── US-40-5 (Документация)
```

## Риски

- **Путь к шрифту** — шрифт лежит в `Editor/res/`, но gui backend в `core/engine/`. Нужно передать путь через конфигурацию или resource system. Митигация: использовать `cfg::` или `res://fonts/fa-solid-900.ttf`.
- **Размер шрифта** — ~400KB. Для desktop-приложения несущественно.
- **Обновление Font Awesome** — при обновлении версии нужно обновить и .ttf и header. Митигация: записать версию в комментарии header.

## Критерии завершения эпика

- [ ] Иконочный шрифт загружается и отображается корректно при любом DPI
- [ ] Не менее 15 иконок используются в UI редактора
- [ ] Graceful fallback при отсутствии файла шрифта
- [ ] Документация обновлена
