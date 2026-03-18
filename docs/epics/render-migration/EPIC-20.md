# EPIC-20: Game Loop Migration — миграция игрового цикла

**Theme:** render-migration
**Status:** done
**Depends on:** EPIC-19

## Цель

Перевести `gs_loop_service` со старого монолитного `renderer_3d::on_render()` на новый
конвейер `frame_assembler` + `render_system::render_frame()`.
После этого обе точки входа (редактор и игровой цикл) используют одну архитектуру.

---

## Контекст

Сейчас два параллельных пути рендеринга:

```
Редактор (новый конвейер, работает после EPIC-19):
  edt_frame_loop_service::on_step()
    → frame_assembler.build_frame(context)
    → render_system.render_frame(context)

Игровой цикл (старый путь, работает):
  gs_loop_service::on_step()
    → render_system.render()  ← прямой вызов renderer_3d
```

---

## US-20-1: Подключить `frame_assembler` к `gs_loop_service`

**Файл:** `core/engine/game_system/gs_loop_service.cpp`
**Приоритет:** ВЫСОКИЙ

```cpp
// Было:
render_system.render();  // → renderer_3d::on_render()

// Должно быть:
rnd::frame_context context;
frame_assembler.build_frame(context);
render_system.render_frame(context);
```

`gs_loop_service` получает `frame_assembler` через `data.require<rnd::frame_assembler>()` в `initialize_services`.

**AC:**
- Игровой цикл использует `frame_assembler` + `render_frame`
- Старый вызов `render_system.render()` удалён или закомментирован с пометкой "legacy"

---

## US-20-2: Визуальное регрессионное тестирование

**Приоритет:** ВЫСОКИЙ
**Зависимости:** US-20-1

Сравнить рендер до и после миграции для каждого типа объектов:

| Сценарий | Проверка |
|---|---|
| Backpack (opaque, текстуры) | Корректные диффуз, спекуляр, нормали |
| Skybox | Отображается за геометрией |
| Прозрачные объекты | Нет артефактов сортировки |
| Анимированные меши | Скиннинг работает (если поддерживается pass'ом) |
| Отладочные нормали | Активируются / деактивируются без краша |

> **Замечание по скиннингу:** если `opaque_pass` не поддерживает skinned draws,
> нужно либо добавить параметр `is_skinned` в `draw_call_t` и выделить `skinned_opaque_pass`,
> либо обработать это внутри `opaque_pass` как отдельную ветку.

**AC:**
- Нет визуальных регрессий по сравнению со старым `renderer_3d`

---

## Критерии готовности

- [x] `gs_loop_service` использует `frame_assembler` + `render_system::render_frame`
- [x] Старый `render_system.render()` не вызывается нигде
- [x] Визуальный результат идентичен старому конвейеру (все сценарии из US-20-2)
