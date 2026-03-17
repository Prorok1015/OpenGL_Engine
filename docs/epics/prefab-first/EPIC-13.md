# EPIC-13: Remove Prototype Descs

**Theme:** prefab-first
**Status:** done
**Depends on:** EPIC-12

## Цель

Удалить все `*_prototype_desc` классы — монолитные объекты, заменённые desc-driven иерархией.
После EPIC-12 ни один путь загрузки моделей не использует прототипы — их код мёртвый.

**Удаляемые классы:**
| Файл | Класс | Причина |
|---|---|---|
| `scn_prototype_desc.h/.cpp` | `prototype_desc` | базовый прототип с `load_prototype` |
| `scn_animatable_prototype_desc.h/.cpp` | `animatable_prototype_desc` | прототип + кости/анимации |
| `scn_skinning_prototype_desc.h/.cpp` | `skinning_prototype_desc` | скиннинг на прототипе |

> `scn_skin_prototype_desc` — это тип `"skin_prototype_desc"` регистрируемый в адаптере напрямую,
> он живёт не в отдельном `.h` — его удаление покрывается EPIC-12 US-12-3.

---

## US-13-1: Найти все usages прототипных десков

**Команда поиска:**
```
grep -r "prototype_desc" core/ Editor/
```

Ожидаемые места:
- `scn_scene_service_init.cpp` — регистрация фабрик в `desc_system`
- `gs_game_init.cpp` — регистрация spawner'ов в `ecs_assembler`
- `scn_animatable_prototype_desc.h` — объявление базового класса
- Потенциально: тесты в `unittests/`

**AC:**
- Полный список файлов с зависимостями задокументирован (добавить в этот файл)

---

## US-13-2: Убрать регистрации из `scn_scene_service_init.cpp`

**Файлы:** `core/engine/scene/scn_scene_service_init.cpp`

Удалить:
- `desc_system.register_type<scn::prototype_desc>("prototype_desc")`
- `desc_system.register_type<scn::animatable_prototype_desc>("animatable_prototype_desc")`
- `desc_system.register_type<scn::skinning_prototype_desc>("skinning_prototype_desc")`
- Соответствующие `#include`

**AC:**
- Компилируется без прототипных десков в desc_system

---

## US-13-3: Убрать spawner'ы из `gs_game_init.cpp`

**Файлы:** `core/engine/game_system/gs_game_init.cpp`

Удалить регистрации spawner'ов для `prototype_desc`, `animatable_prototype_desc`,
`skinning_prototype_desc` и связанные `#include`.

**AC:**
- Компилируется без prototype spawner'ов

---

## US-13-4: Удалить файлы прототипных десков

Удалить:
- `core/engine/scene/scn_prototype_desc.h`
- `core/engine/scene/scn_prototype_desc.cpp`
- `core/engine/scene/scn_animatable_prototype_desc.h`
- `core/engine/scene/scn_animatable_prototype_desc.cpp`
- `core/engine/scene/scn_skinning_prototype_desc.h`
- `core/engine/scene/scn_skinning_prototype_desc.cpp`

**AC:**
- Файлы удалены
- Полный билд проходит без ошибок

---

## US-13-5: Удалить `load_prototype` из `ecs_assembler` (если есть)

**Файлы:** `scn_ecs_assembler.h/.cpp`

Если `ecs_assembler` имеет отдельную ветку обработки `load_prototype` — убрать её.
Весь ввод теперь идёт через `assemble_and_apply`.

**AC:**
- `ecs_assembler` не содержит упоминаний `prototype_desc`, `load_prototype`

---

## Критерии готовности

- [ ] Удалены файлы `scn_prototype_desc`, `scn_animatable_prototype_desc`, `scn_skinning_prototype_desc`
- [ ] Нет `grep`-совпадений на `prototype_desc` в `core/` и `Editor/` (кроме комментариев)
- [ ] Полный билд (editor + unit_tests) проходит
- [ ] Все загружаемые модели рендерятся как раньше
