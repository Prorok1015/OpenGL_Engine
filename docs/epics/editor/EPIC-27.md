# EPIC-27: Texture Desc — полноценная система текстурных дескрипторов

**Theme:** editor
**Status:** planned
**Depends on:** EPIC-08 (Desc-Driven Core)

---

## Цель

Расширить систему текстурных дескрипторов: поддержать все виды текстур (1D, 2D, 3D, cubemap, array), добавить возможность хранить сырые пиксельные данные inline в `.desc` файле (без внешнего `.png`/`.jpg`), и сделать текстуры полноценными ассетами для редактора.

---

## Проблема текущей архитектуры

Сейчас текстурный дескриптор **всегда ссылается на внешний файл изображения**:

```json
{
  "__type": "texture_2d_desc",
  "data": "res://textures/brick.png"
}
```

**Проблемы:**
1. Нельзя создать текстуру без внешнего файла изображения (например, solid color, checkerboard, noise)
2. `texture_desc` — абстрактный базовый класс, не используется напрямую; нет `texture_1d_desc`, `texture_3d_desc`, `texture_array_desc`
3. При импорте модели embedded-текстуры сохраняются как отдельные `.png`/`.pct` файлы + `.txm.desc` — лишние файлы для простых текстур (solid color fallback)
4. Параметры фильтрации и wrap хранятся в header внутри дескриптора, но нет UI для их редактирования
5. Render target текстуры создаются программно через `generate_texture()` — нет desc для них
6. Cubemap поддерживает только два режима (single panorama / 6 faces) — нет GGX prefilter, irradiance map

**Текущие типы:**

| Тип | Файл | Статус |
|---|---|---|
| `texture_desc` | `rnd_texture_desc.h` | Базовый, абстрактный |
| `texture_2d_desc` | `rnd_texture_2d_desc.h` | Работает, ссылается на `picture_resource` |
| `texture_cubemap_desc` | `rnd_texture_cubemap_desc.h` | Работает, single/6-faces |
| `texture_1d_desc` | — | Не существует |
| `texture_3d_desc` | — | Не существует |
| `texture_2d_array_desc` | — | Не существует |

---

## Архитектура: inline data

Ключевая идея — `data` может быть как ссылкой на внешний файл, так и inline-блоком:

```json
// Ссылка на файл (как сейчас)
{
  "__type": "texture_2d_desc",
  "data": "res://textures/brick.png"
}

// Inline: solid color (4 байта RGBA)
{
  "__type": "texture_2d_desc",
  "header": {
    "data": { "extent": { "width": 1, "height": 1 }, "format": "RGBA8" }
  },
  "inline_data": "base64:AP8AAP8="
}

// Inline: генератор
{
  "__type": "texture_2d_desc",
  "generator": {
    "type": "checkerboard",
    "size": 256,
    "color_a": [255, 255, 255, 255],
    "color_b": [128, 128, 128, 255]
  }
}
```

---

## User Stories

### US-27-1 — Inline pixel data в texture_2d_desc

**Файлы:**
- `core/engine/render/render_system/texture/rnd_texture_2d_desc.h/.cpp`
- `core/engine/render/render_system/rnd_render_system.cpp` — loader

**Изменения в `texture_2d_desc`:**

```cpp
class texture_2d_desc : public texture_desc {
public:
    // Вариант 1: ссылка на picture_resource (как сейчас)
    res::res_handle<res::picture_resource> picture_handle;

    // Вариант 2: inline raw bytes (base64-encoded в JSON)
    std::vector<std::byte> inline_pixels;

    bool has_inline_data() const { return !inline_pixels.empty(); }
};
```

**Десериализация:**
- Если поле `"data"` — строка `"res://..."` → загрузить `picture_resource` (legacy path)
- Если поле `"inline_data"` — base64 строка → декодировать в `inline_pixels`
- `header.data` задаёт `extent` и `format` для inline данных

**Loader в `rnd_render_system.cpp`:**
```cpp
if (desc.has_inline_data()) {
    header.data.initial_data = desc.inline_pixels.data();
    return drv->create_texture(header);
} else {
    // существующий путь через picture_resource
}
```

**Base64 утилита:** добавить `ds::base64_encode` / `ds::base64_decode` в `common/`.

---

### US-27-2 — Генераторы текстур (solid color, checkerboard, noise)

**Файлы:**
- Новый `core/engine/render/render_system/texture/rnd_texture_generators.h/.cpp`
- `core/engine/render/render_system/texture/rnd_texture_2d_desc.h/.cpp`

**Генераторы:**

```cpp
namespace rnd {
    struct texture_generator_params {
        std::string type;  // "solid", "checkerboard", "noise"
        int size = 1;
        ds::color color_a{255, 255, 255, 255};
        ds::color color_b{128, 128, 128, 255};
        int seed = 0;
    };

    std::vector<std::byte> generate_texture_data(
        const texture_generator_params& params,
        glm::ivec2& out_size,
        driver::texture_header::TYPE& out_format);
}
```

**JSON формат:**
```json
{
  "__type": "texture_2d_desc",
  "generator": { "type": "solid", "color_a": [255, 0, 0, 255] }
}
```

**Десериализация:** если есть поле `"generator"` → вызвать `generate_texture_data()` → заполнить `inline_pixels` + `header`.

**Применение:** placeholder текстуры при импорте (вместо создания `__black.png`, `__red.png` через `generate_texture()`), fallback текстуры при ошибках загрузки.

---

### US-27-3 — texture_1d_desc, texture_3d_desc, texture_2d_array_desc

**Файлы:**
- Новый `core/engine/render/render_system/texture/rnd_texture_1d_desc.h/.cpp`
- Новый `core/engine/render/render_system/texture/rnd_texture_3d_desc.h/.cpp`
- Новый `core/engine/render/render_system/texture/rnd_texture_array_desc.h/.cpp`
- `core/engine/render/render_system/rnd_render_service_init.cpp` — регистрация
- `core/engine/render/render_system/rnd_render_system.cpp` — loaders

**texture_1d_desc:**
```cpp
class texture_1d_desc : public texture_desc {
    // Gradient / LUT data — inline или ссылка на файл
    res::res_handle<res::picture_resource> picture_handle;
    std::vector<std::byte> inline_pixels;
};
```
Используется для: color ramps, LUT таблицы, toon shading bands.

**texture_3d_desc:**
```cpp
class texture_3d_desc : public texture_desc {
    // Voxel / volume data — только inline или генератор
    std::vector<std::byte> inline_pixels;
    texture_generator_params generator;  // "noise_3d", "perlin"
};
```
Используется для: volumetric fog, 3D noise, voxel data.

**texture_2d_array_desc:**
```cpp
class texture_2d_array_desc : public texture_desc {
    // Массив слоёв — каждый слой ссылка на picture или inline
    std::vector<res::res_handle<res::picture_resource>> layers;
    // ИЛИ
    std::vector<std::vector<std::byte>> inline_layers;
};
```
Используется для: terrain splat maps, sprite atlases, shadow map arrays.

---

### US-27-4 — Расширение cubemap_desc: inline faces и генераторы

**Файлы:**
- `core/engine/render/render_system/texture/rnd_texture_cubemap_desc.h/.cpp`
- `core/engine/render/render_system/rnd_render_system.cpp` — loader

**Дополнения:**
- Поддержка inline данных для каждого face (base64)
- Генератор `"solid_cubemap"` — один цвет на все faces (для placeholder skybox)
- Формат `"equirectangular"` + runtime convert to cubemap (single HDR panorama → 6 faces)

```json
{
  "__type": "texture_cubemap_desc",
  "mode": "equirectangular",
  "data": "res://skybox/environment.hdr"
}
```

---

### US-27-5 — Render target desc

**Файлы:**
- Новый `core/engine/render/render_system/texture/rnd_render_target_desc.h/.cpp`
- `core/engine/render/render_system/rnd_render_service_init.cpp` — регистрация
- `core/engine/scene/scn_renderer.cpp` — использовать render_target_desc вместо `generate_texture()`

**Render target как desc:**
```json
{
  "__type": "render_target_desc",
  "name": "scene_color",
  "header": {
    "type": "texture_2d",
    "usage": "color_target",
    "data": {
      "extent": { "width": 0, "height": 0 },
      "format": "RGBA16F"
    }
  },
  "size_mode": "viewport",
  "scale": 1.0
}
```

- `size_mode`: `"viewport"` (авто-resize с viewport), `"fixed"` (фиксированный размер)
- `scale`: множитель разрешения (0.5 для half-res, 2.0 для supersampling)
- При `size_mode = "viewport"` `extent` = 0,0 → подставляется размер viewport при создании

**Замена программных текстур:**
```cpp
// Было:
m_txm.generate_texture(tag, {w, h}, RGBA16F, {});

// Стало:
auto rt_desc = m_res.require_sync<rnd::render_target_desc>(tag);
m_txm.create_from_desc(*rt_desc, viewport_size);
```

---

### US-27-6 — Inspector UI для текстурных дескрипторов

**Файлы:**
- Новый `Editor/code/editor_system/edt_component_renderers/edt_cr_texture.cpp`
- `Editor/code/editor_system/edt_component_renderers.cpp` — регистрация

**UI в Inspector при выборе texture_desc:**

```
[ Texture 2D: brick_albedo ]
  Source:  ● File  ○ Inline  ○ Generator
  File: [res://textures/brick.png] [Browse]
  ─────────────────────────────────
  Format:   RGBA8
  Size:     512 x 512
  Mipmaps:  ☑ Auto-generate
  ─────────────────────────────────
  Filtering:
    Min: [Linear  ▼]
    Mag: [Linear  ▼]
  Wrapping:  [Repeat ▼]
  ─────────────────────────────────
  Preview: [256x256 thumbnail]
```

Для generator-текстуры:
```
  Source:  ○ File  ○ Inline  ● Generator
  Type: [Checkerboard ▼]
  Size: [===256===]
  Color A: [████████]
  Color B: [████████]
  Preview: [live preview]
```

---

## Новые файлы

```
common/ds/
└── ds_base64.h/.cpp                             # US-27-1: base64 encode/decode

core/engine/render/render_system/texture/
├── rnd_texture_1d_desc.h/.cpp                    # US-27-3
├── rnd_texture_3d_desc.h/.cpp                    # US-27-3
├── rnd_texture_array_desc.h/.cpp                 # US-27-3
├── rnd_texture_generators.h/.cpp                 # US-27-2
└── rnd_render_target_desc.h/.cpp                 # US-27-5

Editor/code/editor_system/edt_component_renderers/
└── edt_cr_texture.cpp                            # US-27-6
```

---

## Граф зависимостей

```
US-27-1 (Inline data + base64)
  ├── US-27-2 (Генераторы)
  ├── US-27-3 (1D, 3D, array)
  └── US-27-4 (Cubemap расширение)

US-27-5 (Render target desc)  ← standalone

US-27-6 (Inspector UI)  ← после US-27-1..4
```

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1 — Inline data** | US-27-1 | `texture_2d_desc` поддерживает base64 inline pixels |
| **2 — Генераторы** | US-27-2 | solid/checkerboard/noise без внешних файлов |
| **3 — Новые типы** | US-27-3, US-27-4 | 1D, 3D, array, cubemap equirectangular |
| **4 — Render targets** | US-27-5 | RT как desc-ресурсы с auto-resize |
| **5 — Editor UI** | US-27-6 | Inspector для просмотра и редактирования текстур |

---

## Риски

| Проблема | Решение |
|---|---|
| Base64 раздувает JSON | Использовать только для маленьких текстур (< 64KB); большие — внешний файл |
| Обратная совместимость | `"data": "res://..."` продолжает работать; `"inline_data"` и `"generator"` — новые поля |
| 3D/array текстуры не поддержаны в OpenGL driver | `rnd_gl_texture.cpp` уже поддерживает `GL_TEXTURE_3D`, `GL_TEXTURE_2D_ARRAY` через `texture_header::TEXTURE_TYPE` |
| Equirectangular → cubemap conversion | Compute shader или CPU-side conversion при загрузке |
| Render target resize при изменении viewport | `texture_manager` вызывает `drv->create_texture()` с новым size при resize |

---

## Критерии готовности

- [ ] `texture_2d_desc` с `"inline_data"` загружается и рендерится
- [ ] Генератор `"solid"` создаёт 1x1 текстуру заданного цвета без внешнего файла
- [ ] `texture_1d_desc` / `texture_3d_desc` / `texture_2d_array_desc` зарегистрированы и работают
- [ ] Cubemap поддерживает equirectangular HDR панораму
- [ ] Render target описан через `render_target_desc` с auto-resize
- [ ] Inspector показывает preview текстуры и позволяет менять параметры фильтрации/wrap
- [ ] Все существующие `.txm.desc` файлы продолжают загружаться (обратная совместимость)
