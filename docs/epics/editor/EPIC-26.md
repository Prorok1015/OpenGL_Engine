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

**#1200, второй PR:** шаг проекта `engine.material_shaders` (фаза pre, до шагов типов) переводит материалы, которые шаг типа отклоняет: стадии берутся из ключей `shader_*` материала и цепочки `__parent` (файлы проекта и движка), одна тройка — один `.shader.desc` рядом с первым материалом по пути (`-2`, `-3`… при занятом имени, существующий файл не перезаписывается), тройка движка ссылается на его файл. План `--migrate --dry-run` показывает шаг и не прогоняет шаг типа на таких файлах.

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
