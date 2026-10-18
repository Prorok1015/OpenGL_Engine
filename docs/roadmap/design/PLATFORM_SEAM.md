# Дизайн: платформенный шов (эпик #10)

**Issue:** #10 (stories #101–#104) · **Утверждено владельцем:** 2026-10-18 («да», своя ImGui-платформа) · Основа: `docs/roadmap/ARCHITECTURE_REVIEW.md` §3.4, §3.5, §9.2 (GLFW остаётся, SDL3 отклонён)

## 1. Как устроено сейчас

- Окно — модуль `window` (`core/core/windows/window_service/`, namespace `wnd`). `GLFWwindow*` торчит в публичных заголовках (`wnd_window.h:8,38,42`, неявный `operator GLFWwindow*`; `wnd_window_context.h:40`). Таблица клавиш GLFW — в заголовке `wnd_input_keycode_convert.hpp`.
- `wnd::window_system` (создаёт `core_module` через `window_init`) в конструкторе делает `glfwInit`, GL-хинты 4.5 core debug, окно и `gl::render_context(glfwGetProcAddress)` (`wnd_window_system.cpp:122-139`). Поэтому `window` линкует `glad` и `drv_opengl` (`window_service/CMakeLists.txt:19`).
- Контекст создаётся неявно в `glfwCreateWindow`, делается текущим в конструкторе `wnd::window` и в `set_vsync`. `glfwSwapBuffers` — в `wnd::context` (`wnd_window_context.cpp:19`). У драйвера (`core/core/video/drivers/impls/opengl/rnd_gl_render_context.*`) только загрузка glad + `create_driver()`; понятия swapchain нет.
- Стадии кадра (`gs_engine_frame_stages.cpp:21-58`): `input` → `pool_events`, `extract` → `backbuffer_size` из `glfwGetFramebufferSize`, `present` → `process_windows()` (swap) + проверка закрытия.
- События: колбэки GLFW → `window_listener_interface` → `inp::input_system`. Есть key/mouse/scroll/cursor/focus/size/move. **Нет** char, framebuffer resize, content scale. Курсор и resize — в координатах окна, не framebuffer. Content scale читается один раз (только для шрифта ImGui).
- ImGui: один класс `gui::gl::gl_imgui_backend` (`core/engine/gui/backends/opengl/`) = stock `imgui_impl_glfw` (`InitForOpenGL`, `install_callbacks=true`, цепляется поверх колбэков движка) + `imgui_impl_opengl3`. Получает `GLFWwindow*` как `void*`. `set_display_size`/`set_input` — мёртвый код.
- Headless — Xvfb + GLX (Mesa llvmpipe), `tools/run_headless.sh`. Должен продолжать работать без изменений.
- Пути в трекере (`core/core/window/`, `core/drivers/drv_opengl/`, `Editor/code/imgui/`) устарели — реальные указаны выше; issues поправить.

## 2. Решение

**Namespace и место остаются** (`wnd`, `core/core/windows/window_service/`): по правилу 2–4 буквы, и это меньше переименований. Меняется содержимое публичных заголовков, а не адреса.

**Интерфейсы без GLFW** (#101):
- `wnd::window` — абстрактный: `framebuffer_size()`, `window_size()`, `content_scale()`, `native_handle()` (непрозрачный `void*` для бэкендов), заголовок, иконка, режим курсора, should-close.
- `wnd::window_system` — абстрактный: `poll_events()`, активное окно, слушатели событий (API слушателя прежний, `inp::input_system` не меняется).
- Реализация — `wnd::glfw_window` / `glfw_window_system` в `.cpp`/внутренних заголовках модуля; таблица клавиш уходит в `.cpp`. Публичные заголовки не включают GLFW.

**Контекст и swap — в драйвере, вызовы GLFW — через поверхность** (#102):
- Окно отдаёт `wnd::gl_surface` (интерфейс без GLFW-типов): `make_current()`, `swap_buffers()`, `set_swap_interval(int)`, `get_proc_address(const char*)`.
- `drv_opengl` получает `create_swapchain(wnd::gl_surface&)` → `gl::swapchain`: делает контекст текущим, грузит glad через `get_proc_address`, владеет `present()` и vsync. `render_context` превращается в эту точку входа.
- Окно создаётся с хинтами клиентского API из описания окна (`graphics_api::opengl` — 4.5 core debug; `none` — `GLFW_NO_API` на будущее). GLFW не умеет прикрепить контекст к окну без API, поэтому контекст по-прежнему рождается в `glfwCreateWindow`, а драйвер им управляет.
- Итог: `window` не линкует `glad`/`drv_opengl`; стадия `present` зовёт `swapchain.present()`; композиция (`rnd_render_service_init`) связывает окно и драйвер.

**События и координаты** (#103):
- Новые payload'ы в `core/core/input/`: `char_event` (UTF-32), `framebuffer_resized`, `content_scale_changed`; колбэки `glfwSetCharCallback`, `glfwSetFramebufferSizeCallback`, `glfwSetWindowContentScaleCallback`.
- Курсор в событиях — в координатах framebuffer (позиция окна × framebuffer/window). При 100% DPI значения не меняются.
- Редактор: прямоугольники `input_area` берутся из ImGui в координатах окна — переводить их умножением на `DisplayFramebufferScale` в одном месте (`edt_input_manager`).
- Content scale при смене монитора — пересборка шрифта ImGui по событию.

**ImGui на две половины** (#104):
- `gui::platform_backend` — **свой**, без GLFW: питается событиями `wnd` (key/char/mouse/scroll/focus/cursor leave) через тот же слушатель, что и `inp::input_system`; `DisplaySize`/`DisplayFramebufferScale` и `dt` — из `wnd::window`; клавиши — наши коды → `ImGuiKey`; форма курсора и буфер обмена — через `wnd::window` (`set_cursor_shape`, `get/set_clipboard`). `imgui_impl_glfw` уходит. Один путь событий: кто получает ввод (ImGui или слои движка, `WantCapture*`) решается в одном месте; UI можно гонять подставными событиями в тестах.
- Не переносим сейчас: геймпад (`NavEnableGamepad` снимается до задачи про геймпад в шве) и multi-viewport (не включён; docking работает).
- `gui::renderer_backend` — `imgui_impl_opengl3` + `get_imgui_texture_from_texture` (без GLFW).
- `gui_system` держит обе; собирает их `gui_init` (единственное место, где названы конкретные бэкенды). Мёртвые `set_display_size`/`set_input` удаляются.
- Новые цели CMake добавить в whole-archive список `snk_engine` (`core/engine/CMakeLists.txt:62-64`).

**Тестируемость:** `wnd::null_window_system` (без дисплея, без GL) для unit-тестов композиции и стадий. Реального offscreen-рендера не добавляем — headless остаётся Xvfb+GLX.

## 3. Решения (рекомендации, ждут подтверждения)

1. Namespace `wnd` и текущая папка остаются; отдельного `platform::` нет.
2. Swap/vsync/загрузка glad — у драйвера через `wnd::gl_surface`; контекст рождается вместе с окном по хинтам из описания окна (вариант «драйвер сам создаёт контекст» невозможен на GLFW).
3. Курсор и resize в событиях — framebuffer-координаты; редактор переводит ImGui-прямоугольники одним множителем.
4. ImGui-платформа — своя, на событиях `wnd` (`imgui_impl_glfw` удаляется); renderer — stock `imgui_impl_opengl3`.
5. `null_window_system` — только для тестов.
6. Порядок уничтожения сохраняется (окно до `glfwTerminate`, #182); swapchain разрушается до окна.

## 4. Шаги (каждый — отдельный PR, CI зелёный, headless smoke без изменений)

1. #101: интерфейсы `wnd::window`/`window_system`, GLFW-реализация за ними, keycode-таблица в `.cpp`, `null_window_system` + тест. Поведение не меняется.
2. #102: `wnd::gl_surface`, `gl::swapchain` в `drv_opengl`, `present` через swapchain; `window` перестаёт линковать `glad`/`drv_opengl`.
3. #103: char / framebuffer resize / content scale, курсор в framebuffer-координатах, перевод `input_area`, пересборка шрифта по смене scale; `wnd::window` получает форму курсора и буфер обмена.
4. #104: своя ImGui-платформа на событиях `wnd` + renderer на `imgui_impl_opengl3`; `imgui_impl_glfw` удаляется. **Сделано.** Известные ограничения: клавиш grave/скобок/backslash нет в enum клавиш движка; трансляции клавиш для неамериканских раскладок нет.

Порядок строго последовательный: #104 нужны события и координаты из #103.
