# EPIC-34: Editor Model Importer — расширение и упрощение

**Status:** planned
**Theme:** editor
**Dependencies:** EPIC-22, EPIC-12

---

## Мотивация

`edt_model_importer.cpp` (706 строк) — основной pipeline импорта 3D-моделей в редактор. Текущие проблемы:

1. **~400 строк дублируются с `scn_model_importer_adapter.cpp`.** Одинаковые функции: `convert_to_glm` (4 перегрузки), `decompose_aimatrix`, `store_data`, `process_mesh_geometry`, `build_mesh_skin_weights`, `build_prefab_node`, `find_material_texture`, `process_material`. Баг-фиксы нужно вносить дважды — легко забыть.

2. **Неполный импорт текстур.** Обрабатываются только `DIFFUSE`, `BASE_COLOR`, `SPECULAR`, `NORMALS`. Не импортируются: metalness, roughness, AO, emissive, opacity — а они часто используются в PBR-моделях (glTF).

3. **Нет импорта камер и источников света.** Assimp извлекает `aiCamera` и `aiLight` из моделей, но editor их игнорирует.

4. **Захардкоженный шейдер.** `shader_fragment` всегда `"res://shaders/mix_opaque_trans_scene.frag"`, `queue` всегда `"mix"`. Нет выбора шейдера при импорте.

5. **Нет настроек импорта.** Пользователь не может задать: масштаб, поворот осей, флаги Assimp (FlipUVs, GenNormals и т.д.), фильтр мешей.

6. **Layout определяется по первому мешу.** Если первый меш не имеет UV/tangents, а последующие имеют — данные будут потеряны.

## Архитектурное решение

### Общий код → shared header

Вынести дублированные функции в `core/engine/scene/adapters/scn_assimp_helpers.h`:

```cpp
namespace scn::assimp_helpers {
    glm::quat convert_to_glm(const aiQuaternion&);
    glm::vec3 convert_to_glm(const aiVector3D&);
    glm::vec4 convert_to_glm(const aiColor4D&);
    glm::mat4 convert_to_glm(const aiMatrix4x4&);
    trs_result decompose_aimatrix(const aiMatrix4x4&);
    void store_data(std::vector<std::byte>&, const auto&);

    void process_mesh_geometry(const aiScene*, const aiMesh*, rnd::geometry_desc&);
    void build_mesh_skin_weights(const aiMesh*, const bone_index_map_t&, size_t, weights_t&);

    json::object build_bone_component(const aiScene*, aiNode*, const bone_index_map_t&);
    json::object build_keyframes_component(const std::string& node_name, const node_kf_map_t&);
    json::object build_animations_desc(const aiScene*);
    bone_index_map_t scan_bones(const aiScene*);
    node_kf_map_t build_keyframes_map(const aiScene*);
    rnd::driver::BufferLayout detect_layout(const aiScene*);
}
```

Обе стороны (editor и engine adapter) используют этот shared header, убирая дублирование.

### Import options

```cpp
struct import_options {
    float scale = 1.0f;
    unsigned int assimp_flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
    bool import_cameras = false;
    bool import_lights = false;
    bool flip_uvs = false;
};
```

## User Stories

### US-34-1: Вынести общий код в scn_assimp_helpers
**Файлы:** `core/engine/scene/adapters/scn_assimp_helpers.h` (новый), `core/engine/scene/adapters/scn_assimp_helpers.cpp` (новый), `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`

Извлечь дублированные функции (convert_to_glm, decompose_aimatrix, store_data, process_mesh_geometry, build_mesh_skin_weights) в shared header. Оба импортера переходят на вызовы из `scn::assimp_helpers`.

**AC:**
- [ ] Создан `scn_assimp_helpers.h/cpp` с общими функциями
- [ ] `edt_model_importer.cpp` использует `scn::assimp_helpers::*` вместо локальных копий
- [ ] `scn_model_importer_adapter.cpp` использует `scn::assimp_helpers::*` вместо локальных копий
- [ ] Нет дублированного кода между двумя импортерами (кроме различий в I/O и texture handling)
- [ ] Сборка и импорт модели работают как раньше

### US-34-2: Вынести общую логику построения prefab-ноды
**Файлы:** `core/engine/scene/adapters/scn_assimp_helpers.h`, `core/engine/scene/adapters/scn_assimp_helpers.cpp`, `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`
**Зависимости:** US-34-1

Функции `build_prefab_node`, `scan_bones`, `build_keyframes_map`, `build_animations_desc` содержат идентичную логику, но различаются способом обработки текстур/материалов. Вынести структурный код, оставив материальный pipeline как callback.

```cpp
// Callback для создания материала — отличается между editor и engine
using material_processor_fn = std::function<json::value(const aiScene*, aiMaterial*)>;

json::object build_prefab_node(
    const aiScene*, aiNode*, rnd::geometry_desc&, res::tag geom_tag,
    const bone_index_map_t&, const node_kf_map_t&, weights_t&,
    material_processor_fn process_material);
```

**AC:**
- [ ] `build_prefab_node` принимает callback для обработки материалов
- [ ] `scan_bones`, `build_keyframes_map`, `build_animations_desc` вынесены в helpers
- [ ] Editor и engine adapter передают свои реализации material processing через callback
- [ ] Логика построения дерева (bone, keyframes, mesh, children) не дублируется

### US-34-3: Расширить импорт текстур — PBR карты
**Файлы:** `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/scene/adapters/scn_assimp_helpers.h`
**Зависимости:** US-34-1

Добавить импорт текстурных типов, часто встречающихся в PBR-моделях (glTF, FBX).

| Assimp тип | Слот | Шейдерный define |
|------------|------|-----------------|
| `aiTextureType_METALNESS` | 3 | `USE_METALNESS_MAP` |
| `aiTextureType_DIFFUSE_ROUGHNESS` | 4 | `USE_ROUGHNESS_MAP` |
| `aiTextureType_AMBIENT_OCCLUSION` | 5 | `USE_AO_MAP` |
| `aiTextureType_EMISSIVE` | 6 | `USE_EMISSIVE_MAP` |
| `aiTextureType_OPACITY` | 7 | `USE_OPACITY_MAP` |

**AC:**
- [ ] `process_material` обрабатывает как минимум metalness, roughness, AO, emissive, opacity
- [ ] Для каждого типа генерируется корректный sampler slot и shader define
- [ ] Пустые промежуточные слоты заполняются `nullptr` (не оставляют дыр)
- [ ] Модели с PBR-текстурами (glTF) импортируются с корректными картами

### US-34-4: Layout определяется по всем мешам
**Файлы:** `core/engine/scene/adapters/scn_assimp_helpers.h`, `core/engine/scene/adapters/scn_assimp_helpers.cpp`
**Зависимости:** US-34-1

Сейчас layout берётся из первого меша. Если первый меш без UV, а остальные с UV — UV теряются. Нужно сканировать все меши и строить layout по максимальному набору атрибутов.

**AC:**
- [ ] `detect_layout()` проходит по всем мешам сцены
- [ ] Layout содержит атрибут если хотя бы один меш его имеет
- [ ] Для мешей без атрибута (например без UV) данные заполняются нулями
- [ ] Работает для моделей со смешанными мешами (часть с UV, часть без)

### US-34-5: Import options + UI настроек импорта
**Файлы:** `Editor/code/editor_system/import/edt_model_importer.h`, `Editor/code/editor_system/import/edt_model_importer.cpp`, `Editor/code/editor_system/import/edt_import_options_dialog.h` (новый), `Editor/code/editor_system/import/edt_import_options_dialog.cpp` (новый)

Добавить структуру `import_options` и ImGui-диалог для настройки перед импортом.

```
┌──────────────────────────────────────┐
│  Import Options                      │
├──────────────────────────────────────┤
│  Scale: [1.0        ]               │
│  [x] Triangulate                    │
│  [x] Generate Smooth Normals        │
│  [ ] Flip UVs                       │
│  [ ] Import Cameras                 │
│  [ ] Import Lights                  │
│                                      │
│          [Cancel]   [Import]         │
└──────────────────────────────────────┘
```

**AC:**
- [ ] Создана `import_options` с полями: scale, assimp_flags (чекбоксы), import_cameras, import_lights, flip_uvs
- [ ] `import_background` принимает `import_options`
- [ ] ImGui-диалог позволяет настроить опции перед запуском импорта
- [ ] Scale применяется к root transform префаба
- [ ] Flip UVs добавляет `aiProcess_FlipUVs` к флагам Assimp

### US-34-6: Импорт камер и источников света
**Файлы:** `Editor/code/editor_system/import/edt_model_importer.cpp`, `core/engine/scene/adapters/scn_assimp_helpers.h`
**Зависимости:** US-34-5

Assimp извлекает `aiCamera` и `aiLight`. Нужно генерировать соответствующие `camera_desc` и `directional_light_desc` компоненты в префабе.

**AC:**
- [ ] Если `import_options.import_cameras == true`, камеры из модели создаются как `camera_desc` ноды в префабе
- [ ] Если `import_options.import_lights == true`, источники света создаются как `directional_light_desc` ноды
- [ ] По умолчанию камеры и свет не импортируются (опция выключена)
- [ ] Поддерживаются как минимум directional lights (остальные типы — logWarning и skip)

### US-34-7: Тесты
**Файлы:** `unittests/scn_assimp_helpers_tests.cpp` (новый)
**Зависимости:** US-34-1

**AC:**
- [ ] Тест: `convert_to_glm` для aiMatrix4x4 → glm::mat4 (column-major)
- [ ] Тест: `decompose_aimatrix` round-trip (TRS → matrix → TRS)
- [ ] Тест: `detect_layout` корректно объединяет атрибуты из нескольких мешей
- [ ] Тест: `build_mesh_skin_weights` корректно упаковывает веса

## Порядок выполнения

```
US-34-1 (shared helpers)
  ├── US-34-2 (shared prefab node builder)
  │     └── US-34-6 (камеры и свет)
  ├── US-34-3 (PBR текстуры)
  └── US-34-4 (layout по всем мешам)
US-34-5 (import options UI) — параллельно с US-34-1
US-34-7 (тесты) — по мере выполнения
```

## Риски

- **Shared header в core, Assimp — в engine.** Assimp-хедеры подключаются через engine; если `scn_assimp_helpers` в core — потребуется линковка с Assimp в core. Альтернатива: разместить helpers в engine layer.
- **Совместимость шейдеров.** Новые PBR sampler slots (metalness, roughness, AO) требуют поддержки в фрагментных шейдерах. Без обновления шейдеров новые текстуры не повлияют на рендеринг.
- **Обратная совместимость material_desc.** Новые поля (дополнительные samplers, defines) должны игнорироваться старыми шейдерами.

## Критерии завершения эпика

- [ ] Нет дублированного кода между `edt_model_importer` и `scn_model_importer_adapter`
- [ ] PBR-модель (glTF с metalness/roughness) импортируется с корректными текстурными слотами
- [ ] Layout строится по всем мешам, а не по первому
- [ ] Диалог Import Options доступен перед импортом
- [ ] Камеры и свет импортируются (если включена опция)
- [ ] Юнит-тесты на shared helpers проходят
