# EPIC-26: Shader Desc — шейдеры как desc-ресурсы

**Theme:** editor
**Status:** planned
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
  "geometry": null,
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

**Файлы:**
- Новый `core/engine/scene/scn_shader_desc.h/.cpp`
- `core/engine/game_system/gs_game_init.cpp` — регистрация типа
- Новые файлы `Editor/res/shaders/*.shader.desc`

**Структура:**

```cpp
namespace scn {
    struct shader_parameter_info {
        std::string name;
        enum class param_type { float_val, vec2, vec3, color4, int_val, define };
        param_type type;
        rnd::driver::uniform_data default_value;  // для uniform'ов
        bool default_enabled = false;              // для define'ов
    };

    struct shader_sampler_info {
        std::string name;
        int slot;
    };

    class shader_desc : public desc::desc_base {
    public:
        res::tag vertex_shader;
        res::tag fragment_shader;
        res::tag geometry_shader;  // optional

        std::vector<shader_parameter_info> parameters;
        std::vector<shader_sampler_info> samplers;

        // Собрать shader_config::constant_data из desc + material overrides
        rnd::shader_config::constant_data build_constant_data(
            const boost::json::object& material_params) const;
    };
}
```

**Регистрация:** `gs_game_init.cpp` → `desc_system.register_type<shader_desc>("shader_desc")`.

**Файлы на диске:** конвертировать существующие шейдеры в `.shader.desc`:
- `Editor/res/shaders/scene.shader.desc` (scene.vert + scene.frag)
- `Editor/res/shaders/sky.shader.desc`
- `Editor/res/shaders/transparent.shader.desc`
- `Editor/res/shaders/z_prepass.shader.desc`
- и т.д.

---

### US-26-2 — Миграция `material_desc` на ссылку `shader_desc`

**Файлы:**
- `core/engine/scene/scn_material_desc.h/.cpp` — заменить inline shader fields на `res::tag shader`
- `core/engine/scene/scn_material_desc.cpp` — десериализация: поддержать оба формата (legacy + новый)
- `Editor/res/*.desc` — обновить material файлы

**Изменения в `material_desc`:**

```cpp
// Было:
rnd::shader_config::constant_data cdata;

// Стало:
res::res_handle<shader_desc> shader;  // ссылка на shader_desc
boost::json::object parameters;       // override'ы параметров
std::unordered_map<std::string, res::tag> sampler_bindings;  // name → texture_desc tag
```

**Обратная совместимость:** если в JSON есть `"shader_vertex"` — загружать по-старому (legacy path). Если есть `"shader"` — загружать через `shader_desc`.

**`get_shader_desc()`** собирает `shader_config` из:
1. `shader_desc::build_constant_data(parameters)` — GLSL теги + defines
2. `sampler_bindings` → resolve текстуры → `rdata.samplers`
3. `parameters` → `rdata.uniforms`

---

### US-26-3 — Рефакторинг `shader_config` и `ShaderManager`

**Файлы:**
- `core/engine/render/render_system/shader/rnd_scene_shader_desc.h/.cpp`
- `core/engine/render/render_system/shader/rnd_shader_manager.h/.cpp`

**Изменения:**
- `shader_config::constant_data::program` теперь может инициализироваться из `shader_desc` (удобный конструктор)
- `ShaderManager` кеширует по `res::tag` шейдера + набору активных defines (не по всему `shader_config`)
- Убрать дублирование кеша при одинаковых шейдерах с разными runtime данными

---

### US-26-4 — Конвертация существующих материалов

**Файлы:**
- `Editor/res/shaders/*.shader.desc` — новые файлы
- `Editor/res/*.desc` — обновить material файлы
- `Editor/res/objects/**/*.mat.desc` — обновить импортированные материалы

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
- `Editor/code/editor_system/edt_model_importer.cpp` — `process_material()`

**Изменения:** при импорте модели `process_material()` создаёт `material_desc` со ссылкой на подходящий `shader_desc` вместо inline `shader_vertex`/`shader_fragment`:

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
core/engine/scene/
└── scn_shader_desc.h/.cpp              # US-26-1: shader_desc класс

Editor/code/editor_system/
└── edt_component_renderers/
    └── edt_cr_shader.cpp               # US-26-5: Inspector UI

Editor/res/shaders/
├── scene.shader.desc                    # US-26-4: PBR scene shader
├── scene_skinned.shader.desc            # скелетная анимация
├── sky.shader.desc                      # skybox
├── transparent.shader.desc              # прозрачные объекты
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
| Обратная совместимость material JSON | `material_desc::deserialize` поддерживает оба формата (legacy inline + shader_desc ref) |
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
