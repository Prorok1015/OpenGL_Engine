# EPIC-48: Skeleton & Skinning Pipeline v2

**Status:** planned
**Theme:** render-migration
**Dependencies:** EPIC-33, EPIC-45

---

## Мотивация

Текущая система скелетной анимации и скиннинга имеет архитектурные проблемы в двух плоскостях: **поиск skeleton root** и **загрузка данных на GPU**.

### Проблемы архитектуры скелета

1. **`find_skeleton_root` — хрупкий поиск.** Логика "иди вверх по parent_component пока не встретишь scene_anchor_component" предполагает, что префаб — прямой ребёнок якоря. Если вложить префаб в группу или другой префаб — функция вернёт корень группы, а не корень скелета.

2. **Дублирование `find_skeleton_root`.** Две идентичные копии: `scn_skinning_desc.cpp` и `scn_animation_job.cpp`. Обе с одной и той же проблемой.

3. **`skeleton_component` создаётся не из desc.** `skeleton_component` и `bone_matrices_component` создаются runtime в `assemble_skinning()`, а не из собственного дескриптора. Нет контроля, откуда берётся skeleton root; `bone_count` может "наращиваться" из разных мешей непредсказуемо.

4. **`bone_component` не знает свой skeleton root.** Каждый кадр `update_bone_offsets_system` для каждой кости ходит по иерархии вверх — O(depth) на кость, O(bones × depth) суммарно.

5. **`skinning_component.skeleton_entity` — runtime entity, не desc-driven.** Ссылка на skeleton root записывается в рантайме через `find_skeleton_root`. При re-assemble entity может поменяться.

### Проблемы GPU pipeline

6. **Один SSBO матриц на весь движок, хардкод 128 костей.** `get_bones_matrices_buffer()` создаёт один буфер `sizeof(mat4) * 128`. Если модель имеет больше 128 костей — данные обрежутся. Если в сцене несколько скелетов — все пишут в один буфер.

7. **Upload матриц на GPU на каждый draw call.** `bind_skin()` вызывает `set_data()` для каждого скинированного меша в каждом render pass. При 5 мешах × 3 прохода (z-prepass, opaque, transparent) = до 15 GPU uploads одних и тех же матриц за кадр. Каждый upload — `glBufferSubData`, вызывающий pipeline stall.

8. **Нет batching для нескольких скелетов.** При переключении между draw calls разных скелетов матрицы перезаписываются. Правильный подход — один большой SSBO с матрицами всех скелетов, draw call содержит offset.

9. **`bind_skin` дублируется в 3 render passes.** Одинаковый код в opaque, transparent, z_prepass — при изменении легко забыть один из проходов.

## Архитектурное решение

### Ключевая идея: skeleton_tag как flat-ключ, данные вне ECS-entity

Вместо хранения `bone_matrices_component` на entity и хождения по иерархии для поиска skeleton root, используется `skeleton_tag` (`res::tag`) — уникальный идентификатор скелета, известный при импорте. Все данные скелета живут в flat storage, индексируемом по `skeleton_tag`.

**Никаких entity-ссылок, никакого хождения по иерархии.** Кость знает свой `skeleton_tag` из desc (установлен при импорте). Данные доступны за O(1) через hashmap lookup.

### bone_matrix_storage — scene-уровневый flat storage

```cpp
// scene layer — никакой зависимости от render
struct bone_matrix_storage {
    std::unordered_map<res::tag, std::vector<glm::mat4>> matrices;

    void ensure(res::tag tag, int bone_count) {
        auto& m = matrices[tag];
        if ((int)m.size() < bone_count)
            m.resize(bone_count, glm::mat4{1.0f});
    }
};
```

Размещается в `reg.ctx()` при инициализации уровня. ECS-системы получают его автоматически через `invoker` (fallback на `reg.ctx().emplace<T>()`):

```cpp
void update_bone_offsets_system(
    bone_matrix_storage& storage,    // ← из reg.ctx(), invoker резолвит сам
    entt::view<entt::get_t<const bone_component, const world_transform>> bones_view)
{
    for (auto [ent, bone, transform] : bones_view.each()) {
        storage.matrices[bone.skeleton_tag][bone.index] = transform.world * bone.offset;
    }
}
```

### Обновлённые компоненты

```cpp
struct bone_component {
    res::tag skeleton_tag;       // ← из desc, при импорте. Ключ в bone_matrix_storage
    glm::mat4 offset{1.0};
    int index = -1;
};

struct skinning_component {
    res::tag skinning_tag;       // ← для weights SSBO
    res::tag skeleton_tag;       // ← для bone matrices lookup
};

struct animated_node_component {
    res::tag skeleton_tag;       // ← для animation controller lookup
    std::string node_name;
};
```

`skeleton_component` и `bone_matrices_component` **удаляются из ECS** — их данные живут в `bone_matrix_storage`.

### skeleton_desc — явный дескриптор

```
prefab_root
  ├── skeleton_desc              ← bone_count, skinning_tag
  │     → assemble: регистрирует слот в bone_matrix_storage
  │
  ├── Bone_Hip (bone_desc: skeleton_tag=..., index=0)
  │     └── Bone_Spine (bone_desc: skeleton_tag=..., index=1)
  │
  └── mesh_Skin
        └── skinning_desc (skeleton_tag=..., skinning_tag=...)
```

`skeleton_tag` одинаков для всех костей и мешей одного скелета — устанавливается при импорте. Вложенность префабов не влияет.

### GPU bone matrices batching

```
┌─────────────────────────────────────────────┐
│         Global Bone Matrices SSBO           │
│  [Skeleton0: 64 mat4][Skeleton1: 128 mat4] │
│  offset=0            offset=64              │
└─────────────────────────────────────────────┘

draw_call_t {
    ...
    uint32_t bone_matrix_offset;  // offset в глобальном SSBO
    uint32_t bone_count;          // количество матриц
}
```

- Render extractor читает `bone_matrix_storage` из `reg.ctx()`, собирает все матрицы в один линейный массив, раздаёт offset'ы draw call'ам
- `skinning_manager::upload_frame_data()` — один upload всех матриц за кадр
- Draw call содержит offset — шейдер читает `bones[offset + bone_index]`
- Нет per-draw-call upload, нет pipeline stalls

### Единый bind_for_draw

Вынести логику привязки skinning буферов из render passes в общий метод `skinning_manager::bind_for_draw(draw_call)`. Три прохода вызывают одну функцию.

## User Stories

### US-48-1: bone_matrix_storage — flat storage матриц в registry context
**Файлы:** `core/engine/scene/scn_bone_matrix_storage.h` (новый), `core/engine/scene/scn_model.h`, `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/level/scn_level.cpp`

Создать `bone_matrix_storage` как ctx-ресурс registry. Заменить `bone_matrices_component` на lookup в storage.

**AC:**
- [ ] Создан `bone_matrix_storage` — `unordered_map<res::tag, vector<mat4>>` в scene layer
- [ ] Размещается в `reg.ctx()` при инициализации уровня
- [ ] `update_bone_offsets_system` принимает `bone_matrix_storage&` через invoker, пишет матрицы по `bone.skeleton_tag`
- [ ] `bone_matrices_component` удалён из ECS
- [ ] `skeleton_component` удалён из ECS (данные в storage)

### US-48-2: skeleton_tag в bone_component и animated_node_component
**Файлы:** `core/engine/scene/scn_model.h`, `core/engine/scene/scn_bone_desc.h`, `core/engine/scene/scn_bone_desc.cpp`, `core/engine/scene/scn_animated_node_desc.h`, `core/engine/scene/scn_animated_node_desc.cpp`

Добавить `skeleton_tag` в `bone_component` и `animated_node_component`. Устанавливается из desc при assembly — никакого runtime-поиска.

**AC:**
- [ ] `bone_component` содержит `res::tag skeleton_tag` (из desc)
- [ ] `bone_desc` содержит поле `skeleton_tag`, serialize/deserialize
- [ ] `animated_node_component` содержит `res::tag skeleton_tag` (из desc)
- [ ] `animated_node_desc` содержит поле `skeleton_tag`, serialize/deserialize
- [ ] `assemble_bone` и `assemble_animated_node` записывают `skeleton_tag` из desc — O(1), нет hierarchy walk

### US-48-3: skeleton_desc — явный дескриптор скелета
**Файлы:** `core/engine/scene/scn_skeleton_desc.h` (новый), `core/engine/scene/scn_skeleton_desc.cpp` (новый), `core/engine/scene/scn_skinning_desc.cpp`, `core/engine/game_system/gs_game_init.cpp`
**Зависимости:** US-48-1

Вынести ответственность за создание слота в `bone_matrix_storage` из `assemble_skinning` в отдельный `skeleton_desc`.

**AC:**
- [ ] Создан `skeleton_desc` — desc-ресурс с полями `bone_count`, `skinning_tag`
- [ ] `assemble_skeleton` регистрирует слот в `bone_matrix_storage` из `reg.ctx()`
- [ ] `assemble_skinning` больше не создаёт `skeleton_component` / `bone_matrices_component` и не вызывает `find_skeleton_root`
- [ ] `skinning_component` хранит `skeleton_tag` из desc (вместо `entt::entity skeleton_entity`)
- [ ] Зарегистрирован в `gs_game_init.cpp`

### US-48-4: Удалить find_skeleton_root
**Файлы:** `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/scn_skinning_desc.cpp`
**Зависимости:** US-48-2, US-48-3

Убрать обе копии `find_skeleton_root`. Все потребители используют `skeleton_tag` для flat lookup.

**AC:**
- [ ] `find_skeleton_root` удалён из `scn_animation_job.cpp`
- [ ] `find_skeleton_root` удалён из `scn_skinning_desc.cpp`
- [ ] `update_bone_offsets_system` — O(1) на кость (storage lookup по tag + array write по index)
- [ ] `update_nodes_animation_system` — O(1) lookup контроллера (по `skeleton_tag` из `animated_node_component`)
- [ ] Нет `entt::entity` ссылок между костями/мешами и skeleton root

### US-48-5: Обновить импортёры — генерация skeleton_tag и skeleton_desc
**Файлы:** `core/engine/scene/adapters/scn_model_importer_adapter.cpp`, `Editor/code/editor_system/import/edt_model_importer.cpp`
**Зависимости:** US-48-2, US-48-3

Импортёры генерируют `skeleton_desc` на корне и `skeleton_tag` в `bone_desc`, `skinning_desc`, `animated_node_desc`.

**AC:**
- [ ] Adapter importer генерирует `skeleton_desc` на skeleton root
- [ ] Adapter importer записывает `skeleton_tag` в `bone_desc`, `skinning_desc`, `animated_node_desc`
- [ ] Editor importer генерирует `skeleton_desc` на skeleton root
- [ ] Editor importer записывает `skeleton_tag` в `bone_desc`, `skinning_desc`, `animated_node_desc`
- [ ] `skeleton_tag` = prefab tag (уникален для каждого импортированного скелета)

### US-48-6: Глобальный SSBO матриц — batching всех скелетов
**Файлы:** `core/engine/render/render_system/skinning/rnd_skinning_manager.h`, `core/engine/render/render_system/skinning/rnd_skinning_manager.cpp`, `core/engine/render/render_system/rnd_render_packet.hpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`

Заменить единственный `bones_matrices_buffer` на динамический буфер, вмещающий матрицы всех активных скелетов. Render extractor читает `bone_matrix_storage` из `reg.ctx()` и собирает данные.

**AC:**
- [ ] `skinning_manager` поддерживает глобальный SSBO с матрицами всех скелетов
- [ ] Размер SSBO масштабируется динамически (не хардкод 128)
- [ ] `draw_call_t` содержит `bone_matrix_offset` и `bone_count` вместо `const vector<mat4>*`
- [ ] Render extractor читает `bone_matrix_storage`, собирает линейный массив, раздаёт offset'ы
- [ ] `skinning_manager::upload_frame_data()` загружает все матрицы одним вызовом `set_data`
- [ ] Вызывается один раз после экстракции, до render passes
- [ ] Нет pipeline stalls от per-draw-call uploads

### US-48-7: bind_for_draw — единый метод привязки skinning
**Файлы:** `core/engine/render/render_system/skinning/rnd_skinning_manager.h`, `core/engine/render/render_system/skinning/rnd_skinning_manager.cpp`, `core/engine/render/render_system/passes/rnd_opaque_pass.cpp`, `core/engine/render/render_system/passes/rnd_transparent_pass.cpp`, `core/engine/render/render_system/passes/rnd_z_prepass.cpp`
**Зависимости:** US-48-6

Вынести дублированную логику привязки skinning из трёх render passes в один метод.

**AC:**
- [ ] Создан `skinning_manager::bind_for_draw(driver, draw_call)` — привязывает weights SSBO + передаёт offset в шейдер
- [ ] Три render passes вызывают `bind_for_draw` вместо inline-кода
- [ ] Дублированный код привязки удалён из passes

### US-48-8: Обновить render data extractor для нового pipeline
**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp`
**Зависимости:** US-48-1, US-48-6

Render extractor больше не ищет `bone_matrices_component` на entity. Вместо этого читает `bone_matrix_storage` из `reg.ctx()`, собирает данные для `skinning_manager::upload_frame_data()`.

**AC:**
- [ ] Экстрактор читает `bone_matrix_storage` из `reg.ctx()`
- [ ] Заполняет `bone_matrix_offset` и `bone_count` в draw calls
- [ ] Skeleton debug lines (show_skeleton) генерируются из `bone_matrix_storage`, не из entity-компонентов
- [ ] Нет обращений к `bone_matrices_component` (удалён)

---

## Порядок выполнения

```
US-48-1 (bone_matrix_storage)
  ├── US-48-2 (skeleton_tag в компонентах)
  │     └── US-48-4 (удалить find_skeleton_root)
  │     └── US-48-5 (импортёры)
  ├── US-48-3 (skeleton_desc)
  │     └── US-48-5 (импортёры)
  └── US-48-8 (render extractor)

US-48-6 (глобальный SSBO + upload_frame_data)  ← параллельно с desc-веткой
  └── US-48-7 (bind_for_draw)
  └── US-48-8 (render extractor)

Три ветки:
  1. Flat storage (US-48-1) → desc-компоненты (US-48-2, US-48-3) → удаление find (US-48-4) → импортёры (US-48-5)
  2. GPU batching (US-48-6) → bind_for_draw (US-48-7)
  3. Render extractor (US-48-8) — финальная интеграция, зависит от 1 и 2
```

## Риски

- **`show_skeleton` debug draw:** сейчас skeleton debug lines генерируются из `skeleton_component` + `bone_matrices_component` на entity. Нужно адаптировать для `bone_matrix_storage`. Может потребоваться дополнительная структура для хранения bone hierarchy (parent indices) в storage.
- **Глобальный SSBO размер:** при большом количестве скелетов буфер может быть значительным. Нужна стратегия grow (например, удваивать при нехватке).
- **Шейдеры:** добавление `bone_matrix_offset` потребует изменения в skinning шейдерах (uniform/push constant для offset).
- **animation_controller_component:** остаётся на entity (skeleton root). `animated_node_component` ищет контроллер по `skeleton_tag`. Нужен механизм: либо второй ctx-ресурс `map<tag, entity>` для быстрого поиска entity с контроллером, либо перенос контроллера тоже в flat storage.
- **Обратная совместимость:** существующие `.desc` без `skeleton_desc` и `skeleton_tag` в `bone_desc` потребуют реимпорта моделей.

## Критерии завершения эпика

- [ ] `find_skeleton_root` удалён, нет runtime-поиска skeleton root по иерархии
- [ ] `bone_matrices_component` и `skeleton_component` удалены из ECS
- [ ] Данные матриц живут в flat `bone_matrix_storage` в `reg.ctx()`, индексируемом по `skeleton_tag`
- [ ] Кости и skinning-меши знают свой `skeleton_tag` из desc — O(1) доступ к данным
- [ ] Нет `entt::entity` ссылок между костями/мешами и skeleton root
- [ ] Матрицы всех скелетов загружаются на GPU одним upload за кадр
- [ ] Нет per-draw-call upload матриц (нет pipeline stalls)
- [ ] Вложенные префабы со скелетами работают корректно
- [ ] Нет дублирования кода привязки skinning в render passes
