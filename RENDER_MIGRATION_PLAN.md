# Render System Migration Plan
## Переход к Data-Driven архитектуре

**Дата составления:** 2026-03-14
**Ветка:** aibranch

---

## Текущее состояние (Диагноз)

### Что уже реализовано (каркас)

| Компонент | Файл | Статус | Заметки |
|---|---|---|---|
| `frame_context` | `rnd_frame_context.h` | Минимальная реализация | Обёртка над `ds::app_data_storage`, 9 строк |
| `frame_assembler` | `rnd_frame_assembler.h` | Минимальная реализация | Оркестратор экстракторов, 23 строки |
| `extractor_interface` | `rnd_extractor_interface.h` | Только интерфейс | Чистый виртуальный класс |
| `render_pass_interface` | `rnd_render_pass_interface.h` | Только интерфейс | Чистый виртуальный класс |
| `render_packet_t` | `rnd_render_packet.hpp` | Базовая структура данных | Нет shader-тега, нет данных о свете |
| `scn_render_data_extractor` | `scn_render_data_extractor.cpp/h` | Частичная, с багами | Viewport-баг, нет разделения на opaque/transparent |
| Вектор `render_passes` в `render_system` | `rnd_render_system.h` | Есть контейнер | Конкретных проходов — ноль |
| Вызов нового конвейера | `edt_frame_loop_service.cpp` | Подключен | Вызывает `build_frame` + `render_frame`, но проходы пустые |

### Две параллельные системы рендеринга (проблема)

```
Редактор (новая архитектура, НЕ РАБОТАЕТ):
  edt_frame_loop_service::on_step()
    → frame_assembler.build_frame(context)   ← экстрагирует данные
    → render_system.render_frame(context)    ← проходы отсутствуют → ничего не рисует

Игровой цикл (старая архитектура, РАБОТАЕТ):
  gs_loop_service::on_step()
    → render_system.render()                 ← прямой вызов monolithic renderer_3d
```

### Критические пробелы

1. **Конкретные Render Pass отсутствуют** — весь рендеринг выполняет `scn::renderer_3d::on_render()` (монолит ~300 строк)
2. **Shader-тег** отсутствует в `draw_call_t` — нельзя выбрать шейдер при render pass
3. **Данные о свете** не экстрагируются — `renderer_3d` делает это инлайн
4. **Регистрация** frame_assembler и экстракторов отсутствует в init-коде
5. **Viewport баг** в `scn_render_data_extractor`: хранит `{center.x, center.y, size.x, size.y}`, а нужно `{left, top, right, bottom}`

---

## Архитектура целевого состояния

```
Игровой/Редакторский цикл
  │
  ├─ Фаза 1: level_manager::update()         [ECS системы]
  │
  ├─ Фаза 2: frame_assembler::build_frame()  [CPU, без GPU]
  │    ├─ scn::mesh_extractor               → render_packet_t (opaque + transparent)
  │    ├─ scn::light_extractor              → light_data_t
  │    └─ scn::camera_extractor             (уже часть mesh_extractor)
  │
  └─ Фаза 3: render_system::render_frame()   [GPU, main thread]
       ├─ rnd::z_prepass                    → depth buffer
       ├─ rnd::opaque_pass                  → G-buffer / forward opaque
       ├─ rnd::skybox_pass                  → skybox
       ├─ rnd::transparent_pass             → OIT weighted blending
       ├─ rnd::composition_pass             → transparency compositing
       └─ rnd::normal_debug_pass            (опционально)
```

---

## Эпики и Задачи

---

### EPIC-1: Контракты данных (Data Contracts)
**Цель:** Обогатить структуры данных `render_packet_t` до уровня, достаточного для реализации всех render-проходов без обращения к ECS.

---

#### TASK-1.1: Добавить `material_tag` в `draw_call_t`
**Файл:** `core/engine/render/render_system/rnd_render_packet.hpp`
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** нет

**Описание:**
`draw_call_t` содержит `geometry_tag`, но не содержит информации о материале/шейдере. Render Pass не может выбрать шейдерную программу.

**Что сделать:**
```cpp
struct draw_call_t {
    uint64_t     sort_key;
    res::tag     geometry_tag;
    res::tag     material_tag;     // ← ДОБАВИТЬ: тег материала (.desc файл)
    uint32_t     indices_count;
    uint32_t     vx_begin;
    uint32_t     ind_begin;
    glm::mat4    transform;
};
```

**Критерий готовности:** `draw_call_t` содержит `material_tag`; экстрактор заполняет его из `geometry_component`.

---

#### TASK-1.2: Добавить структуры данных о свете в `frame_context`
**Файл:** Новый файл `core/engine/render/render_system/rnd_light_data.hpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** нет

**Описание:**
Данные о свете сейчас вычисляются инлайн в `renderer_3d::prepare_directional_light()`. Нужно вынести их в независимые POD-структуры для передачи через `frame_context`.

**Что сделать:**
```cpp
namespace rnd {

struct directional_light_t {
    glm::vec3 direction;
    glm::vec3 color;
    float     intensity;
};

struct point_light_t {
    glm::vec3 position;
    glm::vec3 color;
    float     constant;
    float     linear;
    float     quadratic;
};

struct scene_lights_t {
    std::vector<directional_light_t> directional;
    std::vector<point_light_t>       point;
    glm::vec3                        ambient_color;
    float                            ambient_intensity;
};

} // namespace rnd
```

**Критерий готовности:** Структуры объявлены; `scene_lights_t` можно сохранить в `frame_context.data`.

---

#### TASK-1.3: Исправить viewport-баг в `scn_render_data_extractor`
**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** нет

**Описание:**
Экстрактор хранит viewport как `{center.x, center.y, size.x, size.y}`, а `render_packet_t` ожидает `{left, top, right, bottom}`. Render Pass'ы будут устанавливать неправильный viewport.

**Что сделать:**
```cpp
// Было (НЕПРАВИЛЬНО):
packet.camera.viewport = cam.m_viewport;

// Должно быть:
auto& vp = cam.m_viewport;
packet.camera.viewport = glm::ivec4{
    vp.center.x - vp.size.x / 2,   // left
    vp.center.y - vp.size.y / 2,   // top
    vp.center.x + vp.size.x / 2,   // right
    vp.center.y + vp.size.y / 2    // bottom
};
```

**Критерий готовности:** Unit-тест проверяет корректность преобразования viewport.

---

#### TASK-1.4: Разделить draw-calls на opaque и transparent
**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** TASK-1.1

**Описание:**
Все меши попадают в `opaque_draws`. Нужно проверять флаг прозрачности материала и направлять в `transparent_draws`.

**Что сделать:**
- Загружать `material_desc` по тегу (через `desc_system` или кэш)
- Проверять флаг `is_transparent` / `blend_mode`
- Добавлять draw_call в соответствующий вектор пакета

**Критерий готовности:** Прозрачные объекты (стекло, трава) оказываются в `transparent_draws`, непрозрачные — в `opaque_draws`.

---

### EPIC-2: Слой экстракторов (Extractor Layer)
**Цель:** Полностью перенести логику сбора данных из `renderer_3d` в независимые экстракторы.

---

#### TASK-2.1: Реализовать `scn::light_extractor`
**Файлы:** Новые `core/engine/scene/level/scn_light_extractor.h/cpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** TASK-1.2

**Описание:**
Аналог `scn_render_data_extractor`, но для источников света. Читает компоненты `directional_light_component`, `point_light_component` из ECS, конвертирует в `rnd::scene_lights_t` и сохраняет в `frame_context`.

**Что сделать:**
```cpp
class light_extractor : public rnd::extractor_interface {
public:
    explicit light_extractor(scn::level_manager& lm) : level_manager_(lm) {}
    void extract(rnd::frame_context& context) override;
private:
    scn::level_manager& level_manager_;
};
```

**Критерий готовности:** `scene_lights_t` присутствует в `frame_context` после `build_frame`.

---

#### TASK-2.2: Зарегистрировать экстракторы и frame_assembler
**Файлы:** `Editor/code/editor_system/editor_module.cpp`, `core/engine/scene/scn_scene_service_init.cpp`
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** TASK-2.1

**Описание:**
`frame_assembler` создаётся нигде. Экстракторы к нему не подключены. Нужна инициализация в `initialize_services()`.

**Что сделать:**
1. В `scn_scene_service_init` или `editor_module`: создать `frame_assembler`, добавить экстракторы
2. Сохранить через `data.construct<rnd::frame_assembler>()`
3. В `edt_frame_loop_service::initialize` получить его через `data.require<rnd::frame_assembler>()`

**Критерий готовности:** `edt_frame_loop_service::on_step()` успешно вызывает `build_frame` с зарегистрированными экстракторами.

---

#### TASK-2.3: Добавить frustum culling в mesh_extractor (оптимизация)
**Файл:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Приоритет:** НИЗКИЙ
**Зависимости:** TASK-1.3

**Описание:**
Опциональная оптимизация. Отсекать меши вне frustum'а камеры до генерации draw call.

**Критерий готовности:** Объекты за камерой не попадают в `opaque_draws`.

---

### EPIC-3: Конкретные Render Pass'ы
**Цель:** Реализовать все графические проходы, полностью заменив логику `renderer_3d::on_render()`.

> **Стратегия порта:** Каждый pass является прямым портом соответствующего блока из `scn::renderer_3d::on_render()`. Шейдеры не меняем — только перемещаем вызовы драйвера.

---

#### TASK-3.1: Реализовать `rnd::z_prepass`
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_z_prepass.h/cpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** TASK-1.1, EPIC-2

**Описание:**
Первый проход — запись только глубины (без цвета). Соответствует блоку `renderer_3d::z_prepass()`.

**Что сделать:**
```cpp
class z_prepass : public render_pass_interface {
public:
    void execute(frame_context& ctx, driver::driver_interface& drv) override;
};
```
- Получить `vector<render_packet_t>` из `ctx`
- Для каждого пакета: установить viewport, активировать depth-only шейдер (`z_prepass.vert/frag`)
- Итерировать `opaque_draws`, вызывать `drv.draw_indexed()`

**Критерий готовности:** Depth buffer заполняется корректно; тест — рендеринг сцены с только этим проходом даёт правильный depth.

---

#### TASK-3.2: Реализовать `rnd::opaque_pass`
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_opaque_pass.h/cpp`
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** TASK-3.1, TASK-2.1

**Описание:**
Основной проход — рендеринг непрозрачной геометрии с освещением. Порт из `renderer_3d::render_scene()`.

**Что сделать:**
- Получить `vector<render_packet_t>` и `scene_lights_t` из `ctx`
- Резолвить `material_tag` → шейдерная программа через `shader_manager`
- Резолвить `geometry_tag` → VBO/VAO через `geom_manager`
- Загрузить uniforms: view/projection/model matrices, light data
- Вызвать `drv.draw_indexed()` для каждого `draw_call_t` из `opaque_draws`

**Сортировка draw calls** (важно для GPU performance):
```
sort_key = (shader_id << 32) | (geometry_id & 0xFFFFFFFF)
```

**Критерий готовности:** Непрозрачные объекты сцены отображаются с корректным phong-освещением.

---

#### TASK-3.3: Реализовать `rnd::skybox_pass`
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_skybox_pass.h/cpp`
**Приоритет:** СРЕДНИЙ
**Зависимости:** TASK-3.2

**Описание:**
Рендеринг skybox. Порт из `renderer_3d` (блок skybox). Данные о skybox (cubemap tag) нужно добавить в `frame_context` или `render_packet_t`.

**Критерий готовности:** Skybox отображается корректно поверх фона, за всей геометрией.

---

#### TASK-3.4: Реализовать `rnd::transparent_pass`
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_transparent_pass.h/cpp`
**Приоритет:** СРЕДНИЙ
**Зависимости:** TASK-3.2, TASK-1.4

**Описание:**
OIT (Order-Independent Transparency) проход — weighted blending. Порт из `renderer_3d::render_transparent()`.

**Что сделать:**
- Настроить accumulation + revealage render targets
- Отрисовать `transparent_draws` со специальным шейдером (`transparent.vert/frag`)
- Использовать блендинг по формуле McGuire & Bavoil

**Критерий готовности:** Прозрачные объекты (стекло) отображаются с правильным смешиванием без артефактов сортировки.

---

#### TASK-3.5: Реализовать `rnd::composition_pass`
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_composition_pass.h/cpp`
**Приоритет:** СРЕДНИЙ
**Зависимости:** TASK-3.4

**Описание:**
Финальная композиция — смешивание opaque и transparent результатов в итоговый фреймбуфер. Порт из `renderer_3d::compose()`.

**Критерий готовности:** Финальное изображение включает корректно скомпозированную прозрачность.

---

#### TASK-3.6: Реализовать `rnd::normal_debug_pass` (опционально)
**Файлы:** Новые `core/engine/render/render_system/passes/rnd_normal_debug_pass.h/cpp`
**Приоритет:** НИЗКИЙ
**Зависимости:** TASK-3.2

**Описание:**
Отладочный проход — отображение нормалей через geometry shader. Активируется по флагу в редакторе.

---

### EPIC-4: Регистрация и инициализация конвейера
**Цель:** Правильно зарегистрировать все pass'ы в `render_system` и подключить новый конвейер к редактору.

---

#### TASK-4.1: Зарегистрировать render pass'ы в `rnd_render_service_init`
**Файл:** `core/engine/render/render_system/rnd_render_service_init.cpp`
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** EPIC-3

**Описание:**
Создать и зарегистрировать все render pass'ы в `render_system` при инициализации.

**Что сделать:**
```cpp
// В rnd_render_service_init::initialize_services():
auto& rs = data.require<rnd::render_system>();
rs.add_render_pass(std::make_shared<rnd::z_prepass>(...));
rs.add_render_pass(std::make_shared<rnd::opaque_pass>(...));
rs.add_render_pass(std::make_shared<rnd::skybox_pass>(...));
rs.add_render_pass(std::make_shared<rnd::transparent_pass>(...));
rs.add_render_pass(std::make_shared<rnd::composition_pass>(...));
```

**Критерий готовности:** `render_system::render_frame(context)` выполняет все проходы в правильном порядке.

---

#### TASK-4.2: Верификация редакторского конвейера (end-to-end)
**Приоритет:** КРИТИЧЕСКИЙ
**Зависимости:** TASK-4.1, TASK-2.2

**Описание:**
После подключения всех компонентов — проверить, что редактор рендерит сцену через новый конвейер.

**Чеклист:**
- [ ] `edt_frame_loop_service::on_step()` вызывает `build_frame` → данные в `frame_context`
- [ ] `render_system::render_frame(context)` запускает все pass'ы
- [ ] Viewport в редакторе отображает сцену без артефактов
- [ ] Прозрачность работает корректно
- [ ] Skybox отображается

---

### EPIC-5: Миграция игрового цикла
**Цель:** Перевести `gs_loop_service` со старого `renderer_3d::on_render()` на новый конвейер.

---

#### TASK-5.1: Подключить `frame_assembler` к `gs_loop_service`
**Файл:** `core/engine/game_system/gs_loop_service.cpp`
**Приоритет:** ВЫСОКИЙ
**Зависимости:** EPIC-4

**Описание:**
```cpp
// Было:
render_system.render();  // → renderer_3d::on_render()

// Должно быть:
rnd::frame_context context;
frame_assembler.build_frame(context);
render_system.render_frame(context);
```

**Критерий готовности:** Игровой цикл использует новый конвейер; визуальная регрессия отсутствует.

---

#### TASK-5.2: Визуальное регрессионное тестирование
**Приоритет:** ВЫСОКИЙ
**Зависимости:** TASK-5.1

**Описание:**
Сравнить скриншоты до и после миграции:
- Opaque геометрия (backpack model)
- Прозрачные объекты
- Skybox
- Нормали (debug pass)
- Анимированные меши

**Критерий готовности:** Нет визуальных различий между старым и новым конвейером.

---

### EPIC-6: Выведение из эксплуатации legacy renderer
**Цель:** Удалить `scn::renderer_3d` после полной замены его функциональности.

---

#### TASK-6.1: Аудит зависимостей `scn::renderer_3d`
**Файл:** `core/engine/scene/scn_renderer.h/cpp`
**Приоритет:** СРЕДНИЙ
**Зависимости:** EPIC-5

**Описание:**
Найти все места, использующие `scn::renderer_3d` напрямую. Составить список функций, которые нужно перенести.

**Что сделать:**
```bash
grep -r "renderer_3d\|scn_renderer" --include="*.h" --include="*.cpp" .
```

**Критерий готовности:** Список всех зависимостей задокументирован.

---

#### TASK-6.2: Перенести утилиты из `renderer_3d`
**Приоритет:** СРЕДНИЙ
**Зависимости:** TASK-6.1

**Описание:**
Некоторые функции `renderer_3d` (создание render targets, skinning setup) могут быть нужны pass'ам. Перенести их в подходящие места:
- Создание render targets → `render_system` или отдельный `rnd::render_target_manager`
- Skinning data → в `scn_skinning_manager` или отдельный экстрактор

---

#### TASK-6.3: Удалить `scn::renderer_3d`
**Файлы:** `core/engine/scene/scn_renderer.h/cpp`
**Приоритет:** НИЗКИЙ
**Зависимости:** TASK-6.2

**Описание:**
После успешной миграции — удалить файлы `scn_renderer.h/cpp` и все регистрации в init-коде.

**Критерий готовности:** Сборка проходит без `scn_renderer.h`; все тесты зелёные.

---

## Зависимости эпиков (порядок выполнения)

```
EPIC-1 (Data Contracts)
  └─> EPIC-2 (Extractors)
        └─> EPIC-3 (Render Passes)
              └─> EPIC-4 (Wiring)
                    └─> EPIC-5 (Game Loop Migration)
                          └─> EPIC-6 (Legacy Cleanup)
```

Задачи внутри эпиков, где нет зависимостей, можно выполнять параллельно.

---

## Критический путь (MVP — минимально рабочий новый конвейер)

```
TASK-1.1 → TASK-1.3 → TASK-1.2
     ↓
TASK-2.1 + TASK-2.2
     ↓
TASK-3.1 → TASK-3.2 → TASK-3.4 → TASK-3.5
     ↓
TASK-4.1 → TASK-4.2 (редактор рендерит через новый конвейер)
     ↓
TASK-5.1 → TASK-5.2 (игровой цикл мигрирован)
     ↓
TASK-6.x (удаление legacy)
```

---

## Известные риски

| Риск | Вероятность | Влияние | Митигация |
|---|---|---|---|
| Skinning (анимации) несовместимо с новым конвейером | СРЕДНЯЯ | ВЫСОКОЕ | Выделить отдельный `skinned_opaque_pass` или параметр в `draw_call_t` |
| Несоответствие uniform-имён между старыми шейдерами и новыми pass'ами | ВЫСОКАЯ | СРЕДНЕЕ | Строго документировать uniform-контракт каждого шейдера |
| Порядок render pass'ов нарушает depth test | СРЕДНЯЯ | ВЫСОКОЕ | Добавить unit-тест порядка pass'ов с мок-драйвером |
| `ds::app_data_storage` как `frame_context` — медленно из-за выделений | НИЗКАЯ | СРЕДНЕЕ | Профилировать; при необходимости заменить на flat array с type-erasure |
