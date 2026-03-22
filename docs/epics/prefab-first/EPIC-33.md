# EPIC-33: Skinning & Animation Refactoring — префаб-совместимая архитектура

**Status:** in_progress
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
- [x] Создан `skeleton_component { res::tag skeleton_tag; int bone_count; }`
- [x] `assemble_skinning` размещает `bone_matrices_component` на parent-entity (skeleton root), а не на mesh-entity
- [x] `skinning_component` хранит `entt::entity skeleton_entity` для навигации к skeleton root
- [x] При нескольких мешах существует только один `bone_matrices_component` на skeleton root

### US-33-2: Убрать obj_owner_component из анимационного pipeline
**Файлы:** `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`, `core/engine/scene/scn_model.h`
**Зависимости:** US-33-1

`update_bone_offsets_system` и render extractor используют `obj_owner_component` для поиска `bone_matrices_component`. Нужно заменить на навигацию через `parent_component`.

**AC:**
- [x] `update_bone_offsets_system` находит `bone_matrices_component` через `parent_component` иерархию
- [x] `scn_render_data_extractor` находит `bone_matrices` и `skinning_tag` через `parent_component` иерархию
- [x] `obj_owner_component` не используется в анимационном/скиннинг pipeline (если не используется нигде — удалён)

### US-33-3: skinning_tag при assemble + skin_weights_desc
**Файлы:** `core/engine/scene/scn_skinning_desc.h`, `core/engine/scene/scn_skinning_desc.cpp`, `core/engine/scene/scn_skin_weights_desc.h` (новый), `core/engine/scene/scn_skin_weights_desc.cpp` (новый), `core/engine/scene/adapters/scn_model_importer_adapter.cpp`, `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/game_system/gs_game_init.cpp`

Сейчас `skinning_component::skinning_tag` остаётся пустым после assemble. Тег должен прописываться из дескриптора при импорте. Skinning weights должны быть desc-driven (а не регистрироваться только при импорте).

**AC:**
- [x] `skinning_desc` содержит поле `skinning_tag` (res::tag), сериализуется/десериализуется
- [x] При импорте модели `skinning_tag` прописывается в JSON `skinning_desc` (= prefab_tag)
- [x] `assemble_skinning` инициализирует `skinning_component::skinning_tag` из дескриптора
- [x] Render pipeline не заполняет тег неявно — берёт готовое значение из компонента
- [x] Создан `skin_weights_desc` — хранит per-vertex weights, serialize/deserialize JSON
- [x] `assemble_skin_weights` регистрирует веса в `skinning_manager` при сборке
- [x] Импортёры (adapter + editor) генерируют `skin_weights_desc` в prefab JSON вместо direct `register_weights`
- [x] Веса сохраняются в prefab.desc и переживают перезапуск редактора

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
- [x] Создан `animation_controller_component` с полями: current_animation, current_time, speed, playing, loop
- [x] Создан `animation_controller_desc` + `assemble_animation_controller`, зарегистрирован в `gs_game_init.cpp`
- [x] `assemble_animations` автоматически создаёт `animation_controller_component` с первой анимацией
- [x] `update_nodes_animation_system` работает с `animation_controller_component` вместо `playable_animation_component`
- [x] `playable_animation_component` удалён

### US-33-5: Animation clip как shared ресурс
**Файлы:** `core/engine/scene/scn_animation_clip_desc.h` (новый), `core/engine/scene/scn_animation_clip_desc.cpp` (новый), `core/engine/scene/scn_animation_collection_desc.h` (новый), `core/engine/scene/scn_animation_collection_desc.cpp` (новый), `core/engine/scene/scn_animated_node_desc.h` (новый), `core/engine/scene/scn_animated_node_desc.cpp` (новый), `core/engine/scene/scn_model.h`, `core/engine/scene/scn_animation_job.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`, `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/game_system/gs_game_init.cpp`
**Зависимости:** US-33-4

Keyframes дублировались в каждой bone-entity. Вынесены в shared-ресурс — один `animation_clip_desc` на анимацию. `animation_collection_desc` — контейнер клипов с поддержкой inline и file-based ресурсов через `desc_system::get_field_desc`. `animated_node_desc` — маркер для нод с анимационными каналами, проставляется при импорте.

**AC:**
- [x] Создан `animation_clip_desc` — desc-ресурс, содержащий keyframes для всех нод одной анимации
- [x] Создан `animation_collection_desc` — контейнер клипов (`res::res_handle<animation_clip_desc>`) с поддержкой inline/file через `get_field_desc`
- [x] Создан `animated_node_desc` — маркер для нод с анимациями, проставляется при импорте
- [x] При импорте генерируется один `animation_clip_desc` на анимацию (вместо `keyframes_desc` на каждую bone)
- [x] `animation_collection_component` хранит список `res::res_handle<animation_clip_desc>` на skeleton root
- [x] `update_nodes_animation_system` итерирует `animated_node_component`, берёт keyframes из clip по `node_name`
- [x] Удалены `keyframes_desc`, `keyframes_component`, `animations_desc`, `animations_component`
- [x] Оба импортёра (adapter + editor) обновлены для новой архитектуры

### US-33-6: Оптимизация render extractor — убрать копирование матриц
**Файлы:** `core/engine/render/render_system/rnd_render_packet.hpp`, `core/engine/scene/level/scn_render_data_extractor.cpp`, `core/engine/render/render_system/passes/rnd_opaque_pass.cpp`, `core/engine/render/render_system/passes/rnd_transparent_pass.cpp`, `core/engine/render/render_system/passes/rnd_z_prepass.cpp`
**Зависимости:** US-33-1, US-33-2

Каждый draw call копирует вектор bone_matrices (~8KB). Нужно передавать указатель.

**AC:**
- [x] `draw_call_t::bone_matrices` заменён на `const std::vector<glm::mat4>*` (non-owning pointer) или аналог
- [x] Render extractor сохраняет указатель на `bone_matrices_component::matrices`
- [x] Lifetime гарантирован: матрицы живут в registry, rendering в том же кадре
- [x] `rnd_opaque_pass`, `rnd_transparent_pass`, `rnd_z_prepass` обновлены

### US-33-7: Inspector UI для анимаций
**Файлы:** `Editor/code/editor_system/edt_component_renderers/edt_cr_animation.cpp` (новый), `Editor/code/editor_system/edt_component_renderers/edt_cr_skin.cpp`, `Editor/code/editor_system/edt_component_renderers/edt_cr_internal.h`, `Editor/code/editor_system/edt_component_renderers/edt_component_renderers.cpp`
**Зависимости:** US-33-4, US-33-5

Анимационные desc-компоненты не имеют custom renderer'ов в инспекторе (рендерятся generic JSON fallback'ом).

**AC:**
- [x] Component renderer для `animation_controller_desc` — default_animation, speed slider, autoplay, loop checkboxes
- [x] Component renderer для `animation_collection_desc` — read-only список клипов (имя, duration, channel count)
- [x] Renderer для `skinning_desc` расширен — отображает skinning_tag
- [x] Все renderers зарегистрированы в component_ui_registry

### US-33-8: Binary search для keyframes + кеширование индекса
**Файлы:** `core/engine/scene/scn_animation_job.cpp`

`find_keyframe_index()` использует O(n) линейный поиск. Заменить на бинарный, опционально кешировать последний индекс.

**AC:**
- [x] `find_keyframe_index()` использует `std::upper_bound` (O(log n))
- [ ] ~~Опционально: кеширование последнего найденного индекса~~ — отложено, binary search достаточен при текущих масштабах
- [x] Интерполяция корректна при edge cases: 0 ключей, 1 ключ, время ровно на границе кадра

### US-33-9: Юнит-тесты
**Файлы:** `unittests/scn_skinning_animation_tests.cpp` (новый)

**AC:**
- [ ] Тест: сборка skeleton + bone_matrices из desc — `bone_matrices_component` на skeleton root, не на mesh
- [ ] Тест: множественные mesh-entities ссылаются на один `bone_matrices_component`
- [ ] Тест: `animation_controller_component` play/pause/loop корректно управляет current_time
- [ ] Тест: keyframe interpolation edge cases (0 ключей, 1 ключ, граница)
- [ ] Тест: бинарный поиск keyframe возвращает корректный индекс

### US-33-10: Debug Draw Infrastructure — инфраструктура отладочного рендеринга
**Файлы:** `core/engine/render/render_system/rnd_render_packet.hpp`, `core/engine/render/render_system/passes/rnd_debug_pass.h` (новый), `core/engine/render/render_system/passes/rnd_debug_pass.cpp` (новый), `Editor/res/shaders/debug_line.vert` (новый), `Editor/res/shaders/debug_line.frag` (новый), `core/engine/render/render_system/rnd_render_service_init.cpp`

Сейчас в движке нет инфраструктуры для отладочного 3D-рендеринга (линии, точки, wireframes). Гизмо работает через ImGui DrawList (2D overlay), но для костей, коллизий, bounds нужен настоящий 3D debug draw с depth testing.

**AC:**
- [x] Создан `debug_line_t { vec3 start; vec3 end; vec4 color; }` в `rnd_render_packet.hpp`
- [x] Создан `debug_draw_data_t` с `std::pmr::vector<debug_line_t>` в frame_context
- [x] Создан `rnd_debug_pass` — render pass, рисующий линии через `GL_LINES`
- [x] Debug pass использует простой шейдер (position + color, Matrices UBO для VP)
- [x] Depth test включён, depth write выключен (линии не перекрывают друг друга)
- [x] Pass зарегистрирован между transparent и composition
- [x] Линии рендерятся корректно в 3D-пространстве сцены

### US-33-11: Bone Visualization — отображение скелета поверх модели
**Файлы:** `core/engine/scene/level/scn_render_data_extractor.cpp`, `Editor/code/editor_system/edt_component_renderers/edt_cr_skin.cpp`
**Зависимости:** US-33-10, US-33-1

Экстрактор рисует скелет анимированной модели как набор линий (bone → parent). Toggle включается кнопкой "Bones" в toolbar viewport'а.

**AC:**
- [x] Render data extractor генерирует `debug_line_t` для каждой кости (от позиции кости к позиции parent)
- [x] Линии костей отображаются поверх модели (debug pass без depth test)
- [x] Toggle "Bones" в toolbar viewport — появляется при выбранном entity со skeleton_component (или его потомке)
- [x] При отключённом toggle линии не генерируются (zero overhead)

## Порядок выполнения

```
US-33-10 (Debug Draw Infrastructure) — первый приоритет
  └── US-33-11 (Bone Visualization) — после US-33-10 + US-33-1

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

- [x] Анимированная модель корректно рендерится с новой архитектурой (skeleton root + shared bone_matrices)
- [ ] Множественные экземпляры одного префаба анимируются независимо
- [x] Inspector показывает animation controller (speed, autoplay, loop) и animation collection (список клипов)
- [x] `obj_owner_component` удалён из анимационного pipeline
- [x] Нет копирования `bone_matrices` в draw calls
- [x] Debug draw infrastructure позволяет рисовать 3D-линии в сцене
- [x] Скелет анимированной модели визуализируется поверх меша
- [x] Skinning weights сохраняются через desc-систему (переживают перезапуск)
- [ ] Все юнит-тесты проходят
