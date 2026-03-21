# EPIC-45: Render Pipeline Optimization — оптимизация экстракции и рендер пассов

**Status:** planned
**Theme:** render-migration
**Dependencies:** EPIC-43 (Memory Allocators), EPIC-39 (Profiling)

---

## Мотивация

Профайлер (EPIC-39) + аллокаторы (EPIC-43) снизили `ExtractRenderData` с 11ms до ~5ms. Текущий breakdown (79 мешей, одна камера):

| Зона | Время | % кадра | Причина |
|------|-------|---------|---------|
| Extract.ShaderDesc | 1.8ms | 36% | `get_shader_desc()` аллоцирует maps, strings, lookup текстур |
| Extract.PushDraws | 1.9ms | 38% | move `draw_call_t` с тяжёлым `shader_config` внутри |
| Extract.Skinning | 0.06ms | 1% | OK |
| Extract.BuildDC | 0.05ms | 1% | OK |

Главные проблемы:
1. **`shader_config` — тяжёлая структура**: `unordered_map<string,string>`, `vector<string>`, `unordered_map<string,uniform_data>`. Каждый кадр создаётся и копируется/мувится на каждый меш.
2. **Нет кеширования** — экстрактор пересоздаёт всё с нуля каждый кадр, даже если сцена не менялась.
3. **Render passes** — каждый pass делает свои lookup/bind per draw call.

### Цели

- `ExtractRenderData` < 2ms для 100 мешей
- Минимум heap аллокаций на hot path
- Сцена без изменений → extraction ~0 (delta update)

## User Stories

### US-45-1: shader_config как lightweight handle
**Файлы:** `core/engine/render/render_system/shader/rnd_scene_shader_desc.h`, `rnd_scene_shader_desc.cpp`, новый `rnd_shader_config_cache.h/.cpp`

Вместо хранения данных по значению, `shader_config` становится handle на immutable кешированный объект. Материал создаёт config один раз, draw_call хранит только handle. Копирование = копия индекса.

**Дизайн:**
```cpp
// Кеш: immutable shader configs, дедупликация по hash
class shader_config_cache {
    uint32_t intern(shader_config&& cfg);        // → handle
    const shader_config& resolve(uint32_t h);    // handle → data
    void clear_frame();                          // сброс per-frame кеша
};

struct draw_call_t {
    uint32_t shader_config_handle;  // вместо shader_config material
    // ... остальное
};
```

**AC:**
- [ ] `shader_config_cache` с intern/resolve, дедупликация по hash
- [ ] `draw_call_t::material` заменён на `uint32_t shader_config_handle`
- [ ] Render passes используют `resolve(handle)` вместо прямого доступа к material
- [ ] `get_shader_desc()` возвращает handle вместо полной структуры
- [ ] Профайлер: Extract.PushDraws < 0.5ms для 79 мешей

### US-45-2: shader_config на pmr контейнерах
**Файлы:** `core/engine/render/render_system/shader/rnd_scene_shader_desc.h`
**Зависимости:** может делаться параллельно с US-45-1 или вместо

Перевести внутренности `shader_config` на pmr контейнеры с frame allocator:
- `constants`: `std::unordered_map<std::string, std::string>` → flat `pmr::vector<pair<string_view, string_view>>` или `pmr::unordered_map`
- `defines`: `std::vector<std::string>` → `pmr::vector<std::string_view>` (defines — строковые литералы)
- `uniforms`: `std::unordered_map<std::string, uniform_data>` → `pmr::unordered_map` или flat vector

**AC:**
- [ ] `constant_data::constants` не аллоцирует из heap
- [ ] `constant_data::defines` не аллоцирует из heap
- [ ] `runtime_data::uniforms` не аллоцирует из heap
- [ ] `runtime_data::samplers` использует `pmr::vector` или `fixed_vector`
- [ ] Профайлер: Extract.ShaderDesc < 0.5ms для 79 мешей

### US-45-3: Кеширование материалов в экстракторе
**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp/.h`

`get_shader_desc()` вызывается каждый кадр на каждый меш. Если материал не менялся — результат тот же. Кешировать per-entity.

**Дизайн:**
```cpp
// В экстракторе:
unordered_map<entt::entity, cached_shader_info> shader_cache_;
// Инвалидация: dirty flag на material_desc_component
```

**AC:**
- [ ] Shader desc кешируется per-entity
- [ ] Пересоздаётся только при изменении материала (dirty flag)
- [ ] Профайлер: Extract.ShaderDesc < 0.2ms для неизменённой сцены
- [ ] При изменении материала (в Inspector) — корректно инвалидируется

### US-45-4: Delta extraction — dirty-flag система
**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp/.h`, ECS компоненты

Persistent draw list: вместо пересоздания каждый кадр, хранить список draw calls и обновлять только изменённые.

**Дизайн:**
```cpp
// dirty_flag компонент — ставится при изменении transform/material/mesh
struct render_dirty_component {};

// Экстрактор:
// 1. Полная перестройка только при load/reload сцены
// 2. Каждый кадр: обновить только entity с render_dirty_component
// 3. Убрать dirty flag после обработки
```

**AC:**
- [ ] `render_dirty_component` ставится при изменении transform, material, mesh
- [ ] Экстрактор обновляет только dirty entities
- [ ] Статичная сцена: extraction < 0.1ms
- [ ] Корректно работает с add/remove entity
- [ ] Editor: gizmo transform корректно ставит dirty flag

### US-45-5: Render pass batching — сортировка и batch по shader
**Файлы:** `core/engine/render/render_system/passes/rnd_opaque_pass.cpp`, другие passes

Сейчас каждый draw call делает `configure_pass(material)` — потенциально switch shader program. Сортировка по shader program + batch reduces state changes.

**AC:**
- [ ] Opaque draws сортируются по `sort_key` (shader hash) перед рендером
- [ ] `configure_pass` пропускается если shader program не менялся с предыдущего draw call
- [ ] Профайлер: Pass.Opaque снижение > 20% при > 50 draw calls

### US-45-6: Emplace draw calls in-place
**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Зависимости:** US-45-1 или US-45-2

Вместо создания `draw_call_t` на стеке и move в vector, строить напрямую в vector через `emplace_back` + возврат ссылки.

**AC:**
- [ ] Draw calls создаются через `emplace_back` без промежуточных копий
- [ ] Для MIX queue: один draw call создаётся in-place, второй — минимальная копия
- [ ] Профайлер: Extract.PushDraws < 0.3ms для 79 мешей

---

## Порядок выполнения

```
US-45-2 (shader_config на pmr) ← быстрый win, можно сделать первым
  └── US-45-1 (shader_config handle) ← более глубокий рефакторинг

US-45-3 (Кеш материалов)
  └── US-45-4 (Delta extraction) ← самый мощный, но самый сложный

US-45-5 (Batching) ← независимо от остальных
US-45-6 (Emplace) ← после US-45-1 или US-45-2
```

Рекомендуемый порядок: US-45-2 → US-45-6 → US-45-3 → US-45-5 → US-45-1 → US-45-4

## Риски

- **shader_config handle** (US-45-1) — большой рефакторинг, затрагивает все render passes. Митигация: сначала сделать US-45-2 (pmr) для быстрого win.
- **Delta extraction** (US-45-4) — сложная инвалидация, easy to break. Митигация: fallback на полную перестройку, dirty flag должен быть conservative (лучше лишний rebuild чем пропущенный).
- **Batching** (US-45-5) — сортировка добавляет overhead. Митигация: профилировать sort vs state change cost, для < 20 draw calls сортировка может быть дороже.
- **pmr в shader_config** (US-45-2) — shader_config используется за пределами frame lifetime (кеш, editor). Митигация: pmr только для per-frame copies в экстракторе, persistent configs остаются на std allocator.

## Критерии завершения эпика

- [ ] `ExtractRenderData` < 2ms для 100 мешей
- [ ] Статичная сцена (без изменений) < 0.5ms
- [ ] Render passes: state changes минимизированы через batching
- [ ] Нет heap аллокаций на hot path (только frame_allocator)
- [ ] Профайлер подтверждает улучшения (до/после замеры)
