# EPIC-33: Skinning & Animation Refactoring — префаб-совместимая архитектура

**Status:** planned
**Theme:** prefab-first
**Dependencies:** EPIC-12, EPIC-13

---

## Мотивация

Текущая система скиннинга и анимаций была написана до перехода на префабную модель и имеет ряд архитектурных проблем:

1. **Skinning привязан к prefab-тегу, а не к mesh-entity.** `skinning_manager::register_weights()` принимает `prefab_tag` — один набор весов на весь префаб. Если у модели несколько мешей с разным набором костей — все веса свалены в один массив без разграничения.

2. **`obj_owner_component` — костыль.** Меш-entity ищет `bone_matrices_component` и `skinning_component` у "владельца" через `obj_owner_component`, вместо того чтобы использовать иерархию `parent_component`/`children_component`. Это параллельная система ownership, не совместимая с префабной иерархией.

3. **`skinning_component::skinning_tag` не инициализируется при assemble.** В `assemble_skinning()` тег остается пустым (`res::tag{}`), он заполняется только через экстрактор — нарушает data-driven подход.

4. **`bone_matrices_component` на mesh-entity вместо skeleton-root.** При множественных мешах каждый имеет свою копию матриц, хотя скелет один.

5. **Нет управления воспроизведением.** `playable_animation_component` хранит `current_tick` и `name`, но нет механизма: старт, пауза, смена анимации. Компонент не создаётся при assemble.

6. **Keyframes дублируются.** Каждый `keyframes_component` содержит полную копию карты `{anim_name → animation_node}`. При 50+ костях — избыточное дублирование данных.

7. **O(n) поиск keyframe.** `find_keyframe_index()` использует линейный поиск.

8. **Нет inspector UI.** Компоненты `animations_component`, `keyframes_component`, `playable_animation_component` не отображаются и не редактируются в инспекторе.

9. **Копирование матриц в draw call.** Вектор из 128 mat4 (~8KB) копируется по значению на каждый draw call скинированного меша.

## Архитектурное решение

### Новая модель данных

```
prefab_root (skeleton_root)
  ├── skeleton_component          — NEW: ссылка на skeleton_desc ресурс
  ├── bone_matrices_component     — MOVED: живёт только на skeleton root
  ├── animation_controller_component — NEW: заменяет playable_animation
  │
  ├── Bone_Hip (bone_component: index=0)
  │     ├── Bone_Spine (bone_component: index=1)
  │     │     └── ...
  │     ├── Bone_LegL (bone_component: index=2)
  │     └── ...
  │
  └── mesh_Skin (mesh_component + skinning_component)
        └── skinning_component.skeleton_entity → prefab_root
```

### Ключевые изменения

- `skinning_component` хранит ссылку на `skeleton_entity` (вместо `skinning_tag`)
- `bone_matrices_component` живёт на skeleton root, не на mesh-entity
- Keyframes вынесены в shared-ресурс `animation_clip_desc` (один на анимацию)
- `animation_controller_component` заменяет `playable_animation_component` — поддержка старт/стоп/crossfade
- `skinning_tag` передаётся через `skeleton_component` при assemble (на основе prefab-тега)
- Render extractor передаёт указатель/индекс на матрицы, а не копию

## User Stories

### US-33-1: skeleton_component и перенос bone_matrices на skeleton root
**Файлы:** `core/engine/scene/scn_model.h`, `core/engine/scene/scn_skinning_desc.h`, `core/engine/scene/scn_skinning_desc.cpp`, `core/engine/scene/scn_animation_job.cpp`

Сейчас `bone_matrices_component` создаётся на mesh-entity в `assemble_skinning()`. При нескольких мешах каждый хранит свою копию матриц. Нужно вынести `bone_matrices_component` на skeleton root и ввести `skeleton_component` для связи.

**AC:**
- [ ] Создан `skeleton_component { res::tag skeleton_tag; int bone_count; }`
- [ ] `assemble_skinning` размещает `bone_matrices_component` на parent-entity (skeleton root), а не на mesh-entity
- [ ] `skinning_component` хранит `entt::entity skeleton_entity` для навигации к skeleton root
- [ ] При нескольких мешах существует только один `bone_matrices_component` на skeleton root

### US-33-2: Убрать obj_owner_component из анимационного pipeline
**Файлы:** `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`, `core/engine/scene/scn_model.h`
**Зависимости:** US-33-1

`update_bone_offsets_system` и render extractor используют `obj_owner_component` для поиска `bone_matrices_component`. Нужно заменить на навигацию через `parent_component`.

**AC:**
- [ ] `update_bone_offsets_system` находит `bone_matrices_component` через `parent_component` иерархию
- [ ] `scn_render_data_extractor` находит `bone_matrices` и `skinning_tag` через `parent_component` иерархию
- [ ] `obj_owner_component` не используется в анимационном/скиннинг pipeline (если не используется нигде — удалён)

### US-33-3: skinning_tag при assemble
**Файлы:** `core/engine/scene/scn_skinning_desc.h`, `core/engine/scene/scn_skinning_desc.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`

Сейчас `skinning_component::skinning_tag` остаётся пустым после assemble. Тег должен прописываться из дескриптора при импорте.

**AC:**
- [ ] `skinning_desc` содержит поле `skinning_tag` (res::tag), сериализуется/десериализуется
- [ ] При импорте модели `skinning_tag` прописывается в JSON `skinning_desc` (= prefab_tag)
- [ ] `assemble_skinning` инициализирует `skinning_component::skinning_tag` из дескриптора
- [ ] Render pipeline не заполняет тег неявно — берёт готовое значение из компонента

### US-33-4: animation_controller_component — управление воспроизведением
**Файлы:** `core/engine/scene/scn_model.h`, `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/scn_animation_controller_desc.h` (новый), `core/engine/scene/scn_animation_controller_desc.cpp` (новый), `core/engine/game_system/gs_game_init.cpp`

`playable_animation_component` не создаётся при assemble и не поддерживает play/pause/speed. Нужен полноценный контроллер.

```cpp
struct animation_controller_component {
    std::string current_animation;
    float current_time = 0.f;
    float speed = 1.f;
    bool playing = false;
    bool loop = true;
};
```

**AC:**
- [ ] Создан `animation_controller_component` с полями: current_animation, current_time, speed, playing, loop
- [ ] Создан `animation_controller_desc` + `assemble_animation_controller`, зарегистрирован в `gs_game_init.cpp`
- [ ] `assemble_animations` автоматически создаёт `animation_controller_component` с первой анимацией
- [ ] `update_nodes_animation_system` работает с `animation_controller_component` вместо `playable_animation_component`
- [ ] `playable_animation_component` удалён

### US-33-5: Animation clip как shared ресурс
**Файлы:** `core/engine/scene/scn_keyframes_desc.h`, `core/engine/scene/scn_keyframes_desc.cpp`, `core/engine/scene/scn_mesh_nodes.hpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`
**Зависимости:** US-33-4

Keyframes дублируются в каждой bone-entity. Нужно вынести в shared-ресурс — один `animation_clip_desc` на анимацию.

**AC:**
- [ ] Создан `animation_clip_desc` — desc-ресурс, содержащий keyframes для всех нод одной анимации
- [ ] При импорте генерируется один `animation_clip_desc` на анимацию (вместо `keyframes_desc` на каждую bone)
- [ ] `skeleton_component` хранит список `res::res_handle<animation_clip_desc>`
- [ ] `update_nodes_animation_system` берёт keyframes из clip по имени ноды
- [ ] Обратная совместимость: `keyframes_desc` продолжает десериализоваться (fallback)

### US-33-6: Оптимизация render extractor — убрать копирование матриц
**Файлы:** `core/engine/render/render_system/rnd_render_packet.hpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`, `core/engine/render/render_system/passes/rnd_opaque_pass.cpp`, `core/engine/render/render_system/passes/rnd_transparent_pass.cpp`, `core/engine/render/render_system/passes/rnd_z_prepass.cpp`
**Зависимости:** US-33-1, US-33-2

Каждый draw call копирует вектор bone_matrices (~8KB). Нужно передавать указатель.

**AC:**
- [ ] `draw_call_t::bone_matrices` заменён на `const std::vector<glm::mat4>*` (non-owning pointer) или аналог
- [ ] Render extractor сохраняет указатель на `bone_matrices_component::matrices`
- [ ] Lifetime гарантирован: матрицы живут в registry, rendering в том же кадре
- [ ] `rnd_opaque_pass`, `rnd_transparent_pass`, `rnd_z_prepass` обновлены

### US-33-7: Inspector UI для анимаций
**Файлы:** `Editor/code/editor_system/edt_component_renderers/edt_cr_skin.cpp`, `Editor/code/editor_system/edt_component_renderers/edt_cr_animation.cpp` (новый), `core/engine/game_system/gs_game_init.cpp`
**Зависимости:** US-33-4

Анимационные компоненты не отображаются в инспекторе. Нужны component renderers.

**AC:**
- [ ] Component renderer для `animations_component` — отображает список доступных анимаций
- [ ] Component renderer для `animation_controller_component` — play/pause/stop, выбор анимации, speed slider, progress bar
- [ ] Component renderer для `skeleton_component` — bone count, skeleton tag
- [ ] Все renderers зарегистрированы и отображаются в инспекторе при выборе анимированной entity

### US-33-8: Binary search для keyframes + кеширование индекса
**Файлы:** `core/engine/scene/scn_animation_job.cpp`

`find_keyframe_index()` использует O(n) линейный поиск. Заменить на бинарный, опционально кешировать последний индекс.

**AC:**
- [ ] `find_keyframe_index()` использует `std::lower_bound` (O(log n))
- [ ] Опционально: кеширование последнего найденного индекса в `animation_controller_component` для sequential playback
- [ ] Интерполяция корректна при edge cases: 0 ключей, 1 ключ, время ровно на границе кадра

### US-33-9: Юнит-тесты
**Файлы:** `unittests/scn_skinning_animation_tests.cpp` (новый)

**AC:**
- [ ] Тест: сборка skeleton + bone_matrices из desc — `bone_matrices_component` на skeleton root, не на mesh
- [ ] Тест: множественные mesh-entities ссылаются на один `bone_matrices_component`
- [ ] Тест: `animation_controller_component` play/pause/loop корректно управляет current_time
- [ ] Тест: keyframe interpolation edge cases (0 ключей, 1 ключ, граница)
- [ ] Тест: бинарный поиск keyframe возвращает корректный индекс

## Порядок выполнения

```
US-33-1 (skeleton_component)
  └── US-33-2 (убрать obj_owner)
        └── US-33-6 (оптимизация render)
US-33-3 (skinning_tag при assemble) — параллельно с US-33-1
US-33-4 (animation_controller)
  └── US-33-5 (animation clips как ресурсы)
US-33-7 (inspector UI) — после US-33-4
US-33-8 (binary search) — независимо, в любой момент
US-33-9 (тесты) — по мере выполнения задач
```

## Риски

- **Обратная совместимость desc-файлов:** существующие `.desc` с `keyframes_desc` и `skinning_desc` должны продолжать загружаться. Миграция через fallback в десериализации.
- **obj_owner_component может использоваться не только анимацией.** Проверить все usage перед удалением.
- **Render lifetime:** передача указателей вместо копий требует гарантии, что registry не мутируется между extract и render.

## Критерии завершения эпика

- [ ] Анимированная модель корректно рендерится с новой архитектурой (skeleton root + shared bone_matrices)
- [ ] Множественные экземпляры одного префаба анимируются независимо
- [ ] Inspector показывает animation controller с play/pause/speed
- [ ] `obj_owner_component` удалён из анимационного pipeline
- [ ] Нет копирования `bone_matrices` в draw calls
- [ ] Все юнит-тесты проходят
