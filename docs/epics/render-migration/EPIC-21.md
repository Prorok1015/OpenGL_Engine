# EPIC-21: Legacy Renderer Removal — удаление старого рендерера

**Theme:** render-migration
**Status:** planned
**Depends on:** EPIC-20

## Цель

Удалить `scn::renderer_3d` после полной замены его функциональности render pass'ами.
Убрать мёртвый код, упростить зависимости, избавиться от второй системы рендеринга.

---

## US-21-1: Аудит зависимостей `scn::renderer_3d`

**Файлы:** `core/engine/scene/scn_renderer.h/.cpp`
**Приоритет:** СРЕДНИЙ

Найти все прямые использования `scn::renderer_3d`:

```bash
grep -r "renderer_3d\|scn_renderer" --include="*.h" --include="*.cpp" core/ Editor/
```

Ожидаемые места:
- `scn_scene_service_init.cpp` — регистрация в `app_data_storage`
- `gs_game_init.cpp` — регистрация spawner'а или компонента
- `edt_frame_loop_service` — если остался старый путь

**AC:**
- Полный список файлов задокументирован (обновить этот файл)
- Каждое использование проверено на наличие альтернативы в новом конвейере

---

## US-21-2: Перенести утилиты из `renderer_3d`

**Приоритет:** СРЕДНИЙ
**Зависимости:** US-21-1

Функциональность `renderer_3d`, которую нужно сохранить:

| Блок | Куда переезжает |
|---|---|
| Создание render targets (`__editor_camera_rt`, etc.) | `rnd::render_system` или новый `render_target_manager` |
| Skinning SSBO upload | `rnd::skinning_manager` (уже существует) или `skinned_draw_call_t` |
| Framebuffer setup | `render_pass_interface::on_resize()` или `render_system` |

Каждый перенос — отдельный мелкий PR; не удалять исходный код до верификации.

**AC:**
- Вся нужная функциональность перенесена в другие места
- `renderer_3d` не содержит ничего уникального

---

## US-21-3: Удалить `scn::renderer_3d`

**Файлы:** `core/engine/scene/scn_renderer.h/.cpp`
**Приоритет:** НИЗКИЙ
**Зависимости:** US-21-2

1. Убрать регистрацию в `scn_scene_service_init.cpp`
2. Убрать все `#include "scn_renderer.h"`
3. Удалить файлы `scn_renderer.h/.cpp`
4. Убрать `render_system::render()` если он был proxy к `renderer_3d`

**AC:**
- Сборка проходит без `scn_renderer.h`
- Все unit-тесты зелёные
- `grep -r "renderer_3d"` ничего не находит в `core/` и `Editor/`

---

## Критерии готовности

- [ ] `scn_renderer.h/.cpp` удалены
- [ ] Нет `#include "scn_renderer.h"` в кодовой базе
- [ ] Полный билд (`editor` + `unit_tests`) проходит
- [ ] Все визуальные тесты из EPIC-20 US-20-2 по-прежнему зелёные
