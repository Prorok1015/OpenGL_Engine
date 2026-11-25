# EPIC-26: Shader Desc — шейдеры как desc-ресурсы

**Theme:** editor
**Status:** planned
**Issue:** [#36](https://github.com/Prorok1015/OpenGL_Engine/issues/36)
**Depends on:** EPIC-08 (Desc-Driven Core), EPIC-14 (Component UI Registry)

---

## Цель

Перевести шейдерные программы из inline-конфигурации внутри `material_desc` в самостоятельные `.shader.desc` ресурсы. Шейдер становится first-class ассетом: его можно просматривать в Asset Browser, редактировать в Inspector, переиспользовать между материалами.

---

## Проблема текущей архитектуры

Сейчас шейдерная конфигурация живёт **внутри** `material_desc`:

```cpp
// material_desc содержит:
rnd::shader_config::constant_data cdata;  // shader_tags + defines + constants
```

```json
{
  "__type": "material_desc",
  "shader_vertex": "res://shaders/scene.vert",
  "shader_fragment": "res://shaders/scene.frag",
  "defines": ["USE_NORMAL_MAP"],
  "constants": { "MAX_LIGHTS": "12" },
  "samplers": [...],
  "uniforms": {...}
}
```

**Проблемы:**
1. Шейдер нельзя переиспользовать — каждый материал дублирует `shader_vertex` + `shader_fragment` + `defines`
2. Нет единого места для описания доступных uniform'ов и sampler'ов шейдера — материал сам "знает" какие uniform'ы передать
3. Смена шейдера на другой требует ручного копирования всех полей
4. Невозможно показать в редакторе список доступных шейдеров с их параметрами
5. `ShaderManager` кеширует по хешу `shader_config` — при одинаковых шейдерах в разных материалах создаются дубли если отличается хоть один define

**Целевая архитектура:**

```json
// shader_desc (новый ресурс)
{
  "__type": "shader_desc",
  "name": "PBR Scene",
  "vertex": "res://shaders/scene.vert",
  "fragment": "res://shaders/scene.frag",
  "parameters": {
    "albedo":    { "type": "color4", "default": [1,1,1,1] },
    "shininess": { "type": "float",  "default": 32.0 },
    "USE_NORMAL_MAP": { "type": "define", "default": false }
  },
  "samplers": [
    { "name": "albedoTxm", "slot": 0 },
    { "name": "normalTxm", "slot": 1 }
  ]
}

// material_desc (упрощённый)
{
  "__type": "material_desc",
  "shader": "res://shaders/scene.shader.desc",
  "queue": "opaque",
  "parameters": {
    "albedo": [0.8, 0.2, 0.2, 1.0],
    "shininess": 64.0,
    "USE_NORMAL_MAP": true
  },
  "samplers": {
    "albedoTxm": "res://textures/brick_albedo.txm.desc",
    "normalTxm": "res://textures/brick_normal.txm.desc"
  }
}
```

---

## User Stories

### US-26-1 — `shader_desc` как desc-ресурс

**Issue:** [#1199](https://github.com/Prorok1015/OpenGL_Engine/issues/1199)

**Решение 2026-11-23:** первый шаг поставляет только стадии и defines. `parameters`, `samplers` и `name` отложены до отдельной истории (после перевода материала на `shader_desc`, #1200): их форма зависит от того, как материал будет передавать параметры.

**Файлы:**
- Новый `core/engine/render/render_system/include/snk/render/rnd_shader_desc.h` + `rnd_shader_desc.cpp` — класс `rnd::shader_desc` (в render, рядом с `rnd::material_desc`: его потребляет рендер, а scene и так зависит от render)
- `core/engine/render/render_system/rnd_render_service_init.cpp` — регистрация типа в `engine::render::register_render_descs` (`register_document`, чтобы редактор открывал файл как документ данных)
- Новые файлы `core/res/shaders/*.shader.desc` (+ `.resource`)
- `Editor/code/editor_system/controller/edt_builtin_asset_kinds.cpp` — вид ассета `shader` для `.shader.desc` (двойной клик открывает документ данных)

**Структура (поставлено):**

```cpp
namespace rnd {
    class shader_desc : public desc::schema_desc<shader_desc, desc::schema_kind::desc> {
    public:
        res::tag vertex;
        res::tag fragment;
        res::tag geometry;                 // пустой не пишется
        std::vector<std::string> defines;

        // Программа и defines, как их собирает default_material_compiler из тех же стадий
        // (USE_ANIMATION для скиннинга остаётся делом компилятора). extra_defines — defines материала (#1200):
        // объединение, сортировка, без повторов; при одном имени макроса остаётся запись материала.
        shader_config config(const std::vector<std::string>& extra_defines = {}) const;
    };
}
```

Ключи файла: `vertex`, `fragment`, `geometry`, `defines`; чтение и запись — через схему (`desc::field_attrs`), отдельного адаптера нет: любой `*.desc` грузится через `core::desc::desc_adapter_t`.

**Отложено (будущая история):** `parameters` (uniform'ы и define-переключатели с значениями по умолчанию), `samplers` (имя → слот), `name`.

**Файлы на диске:**
- `core/res/shaders/scene.shader.desc` (scene.vert + scene.frag)
- `core/res/shaders/sky.shader.desc` (sky.vert + sky.frag)
- `core/res/shaders/mix_opaque_trans_scene.shader.desc` (scene.vert + mix_opaque_trans_scene.frag)

---

### US-26-2 — Миграция `material_desc` на ссылку `shader_desc`

**Файлы:**
- `core/engine/render/render_system/include/snk/render/rnd_material_desc.h` (`rnd::material_desc`; `scn_material_desc.h` — только псевдонимы) — заменить inline shader fields на `res::res_handle<shader_desc> shader` (`desc_ref`, ключ `shader`)
- `core/engine/render/render_system/rnd_material_desc.cpp` — `material_desc` v1→v2: шаг типа заменяет известные пары `shader_*` ссылкой `shader` на поставляемый `.shader.desc`, неизвестные пары переводит шаг проекта (`tools/project_migration`); legacy-формат не читается
- `core/engine/render/render_system/shader/rnd_material_compiler.cpp` — собирать программу из `shader_desc::config(mat.defines)`
- `core/res/*.desc` — обновить material файлы

**Изменения в `material_desc`:**

```cpp
// Было:
rnd::shader_config::constant_data cdata;

// Стало:
res::res_handle<shader_desc> shader;  // ссылка на shader_desc
boost::json::object parameters;       // override'ы параметров
std::unordered_map<std::string, res::tag> sampler_bindings;  // name → texture_desc tag
```

**Обратная совместимость:** миграцией, а не чтением двух форматов. Шаг типа `material_desc` (v1→v2) при загрузке заменяет известные пары `shader_vertex/fragment/geometry` ссылкой `shader` на поставляемый `.shader.desc`; для неизвестных пар `.shader.desc` создаёт шаг проекта (`--migrate`). Как грузится материал с неизвестной парой до `--migrate` — открытый вопрос #1200.

**Решения 2026-11-23 (#1200, первый PR):** в `shader` — только ссылка (тег или `{guid, path}`); inline-объект `shader_desc` отклоняется с предупреждением загрузки по `/shader`, поле остаётся пустым (программа общая, копия в каждом материале её размножила бы). `defines` материала остаются в материале и сливаются с defines шейдера в `config(defines)`; сортировка меняет порядок строк `#define`, это намеренно (один набор — один вариант). Шейдер ещё грузится — материал пересобирается в следующем кадре (`compiled_material::complete = false`); не загрузился — ошибка в лог, материал не рисуется и не пересобирается; шейдера нет — предупреждение, материал не рисуется. `shader_desc` при загрузке предупреждает о пропущенной стадии и о расширении не своей стадии.

**#1200, второй PR:** шаг проекта `engine.material_shaders` (фаза pre, до шагов типов) переводит материалы, которые шаг типа отклоняет: стадии берутся из ключей `shader_*` материала и цепочки `__parent` (файлы проекта и движка), одна тройка — один `.shader.desc` рядом с первым материалом по пути (`-2`, `-3`… при занятом имени — `res::free_name`, #1279: файл, его `.resource` или оставшийся `.bin` в любом слое res://, без учёта регистра; существующий файл не перезаписывается), тройка движка ссылается на его файл. План `--migrate --dry-run` показывает шаг и прогоняет шаги типов на копии файла, где стадии отклонённых материалов заменены заглушкой `shader` (#1278): так проверяются остальные объекты файла. Diff такого файла в плане не показывается (diff копии — не то, что будет записано). Файл, на котором копия падает, становится `invalid`, и `--force` его не трогает: шаг `engine.material_shaders` его не выбирает.

**`get_shader_desc()`** собирает `shader_config` из:
1. `shader_desc::build_constant_data(parameters)` — GLSL теги + defines
2. `sampler_bindings` → resolve текстуры → `rdata.samplers`
3. `parameters` → `rdata.uniforms`

---

### US-26-3 — Рефакторинг `shader_config` и `ShaderManager`

**Файлы:**
- `core/engine/render/render_system/include/snk/render/shader/rnd_scene_shader_desc.h`
- `core/engine/render/render_system/include/snk/render/shader/rnd_shader_config_cache.h` (кеш вариантов; прежнего `ShaderManager` больше нет)

**Изменения:**
- `shader_config::constant_data::program` теперь может инициализироваться из `shader_desc` (удобный конструктор)
- `ShaderManager` кеширует по `res::tag` шейдера + набору активных defines (не по всему `shader_config`)
- Убрать дублирование кеша при одинаковых шейдерах с разными runtime данными

---

### US-26-4 — Конвертация существующих материалов

**Файлы:**
- `core/res/shaders/*.shader.desc` — новые файлы
- `core/res/*.desc`, `projects/sandbox/res/**/*.desc` — обновить material файлы
- импортированные материалы (`*.mat.desc` в ресурсах проектов)

**Скрипт/утилита:** ручная или полуавтоматическая конвертация:
1. Для каждой уникальной пары (vertex, fragment) → создать `.shader.desc`
2. В каждом `material_desc` заменить `shader_vertex`/`shader_fragment`/`defines` на `"shader": "res://..."` + `"parameters"`

**Тест:** все существующие сцены загружаются и рендерятся без изменений.

---

### US-26-5 — Inspector UI для shader_desc

**Файлы:**
- Новый `Editor/code/editor_system/edt_component_renderers/edt_cr_shader.cpp`
- `Editor/code/editor_system/edt_component_renderers.cpp` — регистрация

**UI в Inspector при выборе material_desc:**

```
[ Material ]
  Shader: [scene.shader.desc    ▼]  [Open]
  Queue:  Opaque
  ─────────────────────────────────
  Parameters:
    albedo:    [████████] (color picker)
    shininess: [===64.0===]
    ☑ USE_NORMAL_MAP
  Samplers:
    albedoTxm: [brick_albedo.txm.desc]  [Browse]
    normalTxm: [brick_normal.txm.desc]  [Browse]
```

- Dropdown/browse для выбора `shader_desc`
- При смене шейдера — UI автоматически перестраивается по `shader_desc::parameters`
- Параметры, отличающиеся от default, выделены жирным
- Кнопка Reset на каждом параметре

---

### US-26-6 — Модельный импорт: генерация shader_desc ссылок

**Файлы:**
- `core/model_import/mdl_import.cpp` — сборка `material_desc` при импорте

**Изменения:** при импорте модели импортёр создаёт `material_desc` со ссылкой на подходящий `shader_desc` вместо inline `shader_vertex`/`shader_fragment`:

```cpp
// Выбор shader_desc по свойствам материала:
if (has_skinning)
    mat["shader"] = "res://shaders/scene_skinned.shader.desc";
else if (has_normal_map)
    mat["shader"] = "res://shaders/scene.shader.desc";
else
    mat["shader"] = "res://shaders/scene_basic.shader.desc";
```

---

### US-26-C — Ошибки шейдеров в Problems со строкой

**Issue:** #1201

**Решения 2026-11-23:**
- Драйвер отдаёт лог сборки: `driver_interface::create_shader(headers, shader_build_log* log = nullptr)`, `shader_build_log { stages: [{stage, text}], link }`. GL читает лог целиком (`GL_INFO_LOG_LENGTH`), при ошибке компиляции стадии не линкует; null-драйвер кладёт в лог причину отказа своей проверки.
- `#line`: после `#version` и вставленных defines — `#line <строка после #version> 0`; каждый раскрытый include обёрнут в `#line 1 <k>` перед текстом и `#line <следующая строка родителя> <k родителя>` после. `k` — индекс в таблице файлов стадии (`load_result::sources`, 0 — сама стадия; своя таблица у каждой стадии). Каждый include раскрывается каждый раз внутри guard своего файла (`#ifndef SNK_INCLUDE_<FNV-1a тега>` / `#define` до `#line 1 <k>`, `#endif` перед `#line` родителя); лишние копии отсекает препроцессор компилятора, так что include внутри отброшенной группы не прячет файл от include в активной части (#1301). У файла один индекс `k`, сколько бы раз он ни включался. Общий объём раскрытого текста ограничен (4 МБ) — превышение даёт одну проблему `shader/include`. Пропущенная строка include (ошибка) оставляет перевод строки, чтобы номера не съезжали. После каждой строки `#else`/`#elif`/`#endif` (в стадии и в include) нумерация задаётся заново (`#line <следующая строка> <k>`): `#line` в пропущенной компилятором группе не действует.
- Ошибки структурные: `rnd::shader_problem { file, line, column, code, message, program }`, коды `shader/compile`, `shader/link`, `shader/include`, `shader/macro`, `shader/load`. Ошибки препроцессора — в `load_result::problems` (include — в файле, где он написан; макрос — через ту же таблицу `#line`). Строки лога GL разбираются в форматах NVIDIA `k(line)`, Mesa `k:line(col)`, AMD/ANGLE `ERROR: k:line:`; нераспознанная строка — проблема стадии без строки; лог линковки — одна проблема файла вершинной стадии. Лог ограничен: 4 КБ на строку, 256 КБ на лог, 100 проблем на программу и одна «… N more problems». Разбор — ручной, без `std::regex`.
- `ShaderManager` хранит проблемы по варианту программы: успешная сборка их снимает, неудачная заменяет (в том числе когда остаётся прежняя программа). Наружу — `render_system::shader_problems()` и `shader_problems_generation()`.
- Редактор: источник Problems `shader` (`shader_problem_publisher`, кадр редактора, только при смене поколения); `Go to` открывает файл в приложении системы (`platform::open_in_shell`) только если это исходник шейдера (`.glsl .vert .frag .geom .comp .tesc .tese .shader`; include может указывать на скрипт), иначе только запись в лог; переход к строке — #1291. Агент видит записи через `problems.list {source:"shader"}`.
- Hot reload (#1272, #1273): материал подхватывает изменённый `.shader.desc` — `material_instance_cache` следит за шейдером каждого экземпляра; пока новый шейдер грузится или если он не загрузился, рисуется прежний экземпляр; ошибка загрузки идёт в Problems (`shader/load`) один раз на пару (шейдер, материал), а не каждый кадр.

---

### US-26-D — Редактор GLSL в редакторе

**Issue:** #1202 (последующая «Проверка» — #1386)

**Решения 2026-11-24:**
- Виджет — вендоренный goossens/ImGuiColorTextEdit (`lib3dparty/ImGuiColorTextEdit`, подмодуль, закреплён на `e09ea63`: последний коммит, который собирается с ImGui 1.91.7; следующие требуют ImGui 1.92). Цель CMake `color_text_edit` — в `lib3dparty/color_text_edit/CMakeLists.txt`, как third-party без `-Werror`. Подъём закрепления — только вместе с подъёмом ImGui.
- Модель чистая и с тестами: `shader_markers_for` (строки Problems источника `shader` → маркеры по строкам, худшая серьёзность, строки без номера — в баннер), `read_shader_text`/`write_shader_text` (LF в редакторе; перевод строк и BOM файла возвращаются при сохранении; NUL, не-UTF-8 и > 4 МБ отклоняются). Виджет — тонкий адаптер (`shader_text_view`), палитра берётся из семантических цветов темы.
- `shader_source_document` (вид `shader_source`): открытие, грязность по тексту, сохранение (отказ в Play, под блокировкой записи, для файла нижнего слоя и для файла, изменённого на диске), `revert`/`overwrite`, наблюдение за файлом, автосохранение и восстановление после сбоя (#1413: вид `shader_source`, файл, изменённый на диске после автосохранения, возвращается конфликтом Reload/Overwrite). Сохранение делает `store_sync` + `signal_changed`, и ShaderManager пересобирает программы, которые используют файл.
- Файлы нижних слоёв открываются только для чтения. Набор текста уходит в документ после паузы 0,3 с; изменение документа во время набора не затирает набор — человек выбирает Reload / Overwrite.
- Интеграция: вид ассетов и превью на каждый суффикс (двойной клик в браузере ассетов; фильтр по видам по-прежнему называет их `shader`), вкладки, список несохранённого («Shader»), вопрос при закрытии, Exit/New/Open, `Go to` из Problems открывает файл на строке записи (если редактор его открывает, иначе — в приложении системы).
- Только UI: инструментов для агента нет. «Проверка» (компиляция отдельного шейдера) — #1386.
- Дальше: восстановление автосохранения, сохранение, когда файл удалён или переименован снаружи, Undo/Redo через `document_manager`.

---

## Новые файлы

```
core/engine/render/render_system/
├── include/snk/render/rnd_shader_desc.h  # US-26-1: shader_desc класс
└── rnd_shader_desc.cpp

Editor/code/editor_system/
└── edt_component_renderers/
    └── edt_cr_shader.cpp               # US-26-5: Inspector UI

core/res/shaders/
├── scene.shader.desc                    # US-26-1: scene.vert + scene.frag
├── scene_skinned.shader.desc            # скелетная анимация
├── sky.shader.desc                      # US-26-1: skybox
├── mix_opaque_trans_scene.shader.desc   # US-26-1: смешанная очередь
├── z_prepass.shader.desc                # depth prepass
└── ...
```

---

## Граф зависимостей

```
US-26-1 (shader_desc ресурс)
  ├── US-26-2 (материал → shader_desc ссылка)
  │     └── US-26-3 (рефакторинг ShaderManager)
  │     └── US-26-4 (конвертация существующих файлов)
  │     └── US-26-6 (импорт моделей)
  └── US-26-5 (Inspector UI)

EPIC-07 (Material Editor) — после EPIC-26
  Полноценный visual material editor опирается на shader_desc для UI генерации.
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Data model** | US-26-1 | `shader_desc` как desc-ресурс, сериализация |
| **2 — Material migration** | US-26-2, US-26-3 | Материалы ссылаются на `shader_desc`, обратная совместимость |
| **3 — Content migration** | US-26-4, US-26-6 | Все `.desc` файлы и импорт используют новый формат |
| **4 — Editor UI** | US-26-5 | Inspector показывает параметры шейдера из `shader_desc` |

---

## Риски

| Проблема | Решение |
|---|---|
| Обратная совместимость material JSON | миграция `material_desc` v1→v2 (шаг типа + шаг проекта), legacy-формат не читается |
| Шейдер-кеш инвалидация | `ShaderManager` перестраивает кеш-ключ: `shader_desc tag + active defines hash` |
| Runtime overhead от загрузки shader_desc | `shader_desc` кешируется в resource_system; `material_desc::get_shader_desc()` разрешает один раз |
| Много `.shader.desc` файлов | На практике 5-10 уникальных шейдерных программ; defines комбинации решаются через параметры |

---

## Критерии готовности

- [ ] `shader_desc` загружается через desc pipeline как самостоятельный ресурс
- [ ] `material_desc` ссылается на `shader_desc` вместо inline GLSL тегов
- [ ] Legacy material JSON (с `shader_vertex`/`shader_fragment`) продолжает работать
- [ ] Все существующие сцены рендерятся без регрессий
- [ ] Inspector показывает параметры шейдера и позволяет их редактировать
- [ ] Модельный импорт генерирует ссылки на `shader_desc`
