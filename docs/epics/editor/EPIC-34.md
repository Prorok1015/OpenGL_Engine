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
**Файлы:** `core/engine/assimp_importer/scn_assimp_helpers.h`, `core/engine/assimp_importer/scn_assimp_helpers.cpp`
**Зависимости:** US-34-8

Извлечь дублированные функции (convert_to_glm, decompose_aimatrix, store_data, process_mesh_geometry, build_mesh_skin_weights) в shared header внутри модуля `assimp_importer`. Эта US теперь выполняется в рамках US-34-8, но выделена для трекинга.

**AC:**
- [ ] Создан `scn_assimp_helpers.h/cpp` внутри `assimp_importer/`
- [ ] Все `convert_to_glm`, `decompose_aimatrix`, `store_data`, `process_mesh_geometry`, `build_mesh_skin_weights` — только в helpers, нигде больше
- [ ] Сборка и импорт модели работают как раньше

### US-34-2: Вынести общую логику построения prefab-ноды
**Файлы:** `core/engine/assimp_importer/scn_assimp_scene_parser.h`, `core/engine/assimp_importer/scn_assimp_scene_parser.cpp`
**Зависимости:** US-34-8, US-34-1

Функции `build_prefab_node`, `scan_bones`, `build_keyframes_map`, `build_animations_desc`, `find_material_texture`, `process_material` объединены в `scn_assimp_scene_parser`. Различие в текстурном I/O абстрагировано через `texture_store_interface`. Эта US теперь выполняется в рамках US-34-8, но выделена для трекинга.

**AC:**
- [ ] `parse_assimp_scene()` — единая точка парсинга для editor и runtime
- [ ] `texture_store_interface` абстрагирует разницу в текстурном I/O
- [ ] `scan_bones`, `build_keyframes_map`, `build_animations_desc` — только в парсере
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
**Зависимости:** US-34-8

**AC:**
- [ ] Тест: `convert_to_glm` для aiMatrix4x4 → glm::mat4 (column-major)
- [ ] Тест: `decompose_aimatrix` round-trip (TRS → matrix → TRS)
- [ ] Тест: `detect_layout` корректно объединяет атрибуты из нескольких мешей
- [ ] Тест: `build_mesh_skin_weights` корректно упаковывает веса

### US-34-8: Выделить Assimp в опциональный CMake-модуль `assimp_importer`
**Файлы:**
- `core/engine/assimp_importer/CMakeLists.txt` (новый)
- `core/engine/assimp_importer/scn_assimp_scene_parser.h` (новый)
- `core/engine/assimp_importer/scn_assimp_scene_parser.cpp` (новый)
- `core/engine/assimp_importer/scn_assimp_helpers.h` (новый — вместо `scene/adapters/`)
- `core/engine/assimp_importer/scn_assimp_helpers.cpp` (новый)
- `core/engine/assimp_importer/scn_model_importer_adapter.h` (перемещён из `scene/adapters/`)
- `core/engine/assimp_importer/scn_model_importer_adapter.cpp` (перемещён из `scene/adapters/`)
- `core/engine/assimp_importer/scn_assimp_resource_system_wrapper.h` (перемещён из `scene/adapters/`)
- `core/engine/assimp_importer/scn_assimp_resource_system_wrapper.cpp` (перемещён из `scene/adapters/`)
- `core/engine/scene/CMakeLists.txt` (убрать `PRIVATE assimp`)
- `Editor/code/editor_system/CMakeLists.txt` (заменить `PRIVATE assimp` → `PRIVATE assimp_importer`)
- `CMakeLists.txt` (корневой — опция `ENABLE_ASSIMP_IMPORTER`)
**Зависимости:** нет (фундаментальная задача, предшествует US-34-1 и US-34-2)

Сейчас Assimp линкуется в двух местах: `scene` (`target_link_libraries(... PRIVATE assimp)` — с комментарием `#temporary`) и `editor_system` (`PRIVATE assimp`). Парсинг модели дублируется в `scn_model_importer_adapter.cpp` (~600 строк) и `edt_model_importer.cpp` (~700 строк). Нужно:

1. **Создать отдельную static library `assimp_importer`** — единственный таргет, который линкует Assimp.
2. **Перенести весь Assimp-зависимый код** из `scene/adapters/` и `editor_system/import/` в новый модуль.
3. **Объединить дублированный парсинг** в `scn_assimp_scene_parser` — единую точку входа для преобразования `aiScene*` в `parse_result` (prefab JSON + geometry + skinning weights + tags).
4. **Сделать модуль опциональным** через CMake-опцию `ENABLE_ASSIMP_IMPORTER` (по умолчанию `ON`).

#### Архитектура модуля

```
core/engine/assimp_importer/         ← новая static library
├── CMakeLists.txt                   ← target_link_libraries(... PRIVATE assimp PUBLIC engine)
├── scn_assimp_helpers.h/cpp         ← convert_to_glm, decompose, store_data, etc.
├── scn_assimp_scene_parser.h/cpp    ← unified parser: aiScene* → parse_result
├── scn_model_importer_adapter.h/cpp ← runtime adapter (перемещён из scene/adapters/)
└── scn_assimp_resource_system_wrapper.h/cpp ← Assimp IOSystem для res:// (перемещён)
```

#### CMake-интеграция

```cmake
# CMakeLists.txt (корневой или core/engine/)
option(ENABLE_ASSIMP_IMPORTER "Build Assimp model importer module" ON)

if(ENABLE_ASSIMP_IMPORTER)
    add_subdirectory("assimp_importer")
endif()
```

```cmake
# core/engine/assimp_importer/CMakeLists.txt
project(assimp_importer)
add_library(${PROJECT_NAME} STATIC)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE assimp PUBLIC scene PUBLIC render)
target_compile_definitions(${PROJECT_NAME} PUBLIC HAS_ASSIMP_IMPORTER=1)
```

```cmake
# scene/CMakeLists.txt — удалить строку:
# target_link_libraries(${PROJECT_NAME} PRIVATE assimp)

# editor_system/CMakeLists.txt — заменить:
# target_link_libraries(... PRIVATE assimp)
# на:
# target_link_libraries(... PRIVATE assimp_importer)
```

#### Единый парсер — `scn_assimp_scene_parser`

```cpp
namespace scn {

// Callback: как сохранить/зарезолвить текстуру.
// Runtime-адаптер резолвит через res://, editor — через filesystem + memory://.
struct texture_store_interface {
    virtual ~texture_store_interface() = default;
    virtual res::tag store_embedded(std::string_view name,
                                     std::span<const std::byte> data,
                                     glm::ivec2 size, int channels) = 0;
    virtual res::tag resolve_external(std::string_view relative_path) = 0;
    virtual res::tag store_texture_desc(const res::tag& data_tag,
                                         std::string_view name) = 0;
};

struct parse_result {
    json::object                        prefab_json;
    rnd::geometry_desc                  geometry;
    res::tag                            geom_tag;
    std::vector<std::vector<uint32_t>>  skin_weights;
    std::vector<res::tag>               created_tags;
};

// Единая точка парсинга — и editor, и runtime вызывают эту функцию.
parse_result parse_assimp_scene(
    const aiScene* scene,
    const std::string& tag_prefix,
    texture_store_interface& textures,
    res::resource_system& res_sys);

} // namespace scn
```

Обе стороны (editor и runtime adapter) предоставляют свою реализацию `texture_store_interface`:
- **Runtime:** `res_texture_store` — резолвит через `res::resource_system::fetch_data()`
- **Editor:** `filesystem_texture_store` — читает с диска через `std::ifstream`, сохраняет в `memory://`

Вся остальная логика (geometry, materials, bones, keyframes, animations, prefab tree) — **одна реализация** в `scn_assimp_scene_parser.cpp`.

#### Условная регистрация runtime-адаптера

В `gs_game_init.cpp` (или `engine_module`):
```cpp
#if HAS_ASSIMP_IMPORTER
    res_sys.register_adapter(scn::model_importer_adapter::INFO,
        std::make_shared<scn::model_importer_adapter>(desc_sys));
#endif
```

Без `ENABLE_ASSIMP_IMPORTER` — engine собирается без Assimp, runtime-загрузка `.glb`/`.fbx` недоступна, но все остальные ресурсы (desc, textures, geometry) работают как обычно.

#### Что остаётся в `editor_system/import/`

- `edt_model_importer.h/cpp` — тонкая обёртка: читает файл с диска, создаёт `Assimp::Importer`, вызывает `scn::parse_assimp_scene()` с `filesystem_texture_store`, оркестрирует background/main split.
- `edt_filesystem_assimp_io.h/cpp` — Assimp IOSystem для файловой системы (специфика editor).
- `edt_asset_exporter.h/cpp`, `edt_asset_export_dialog.h/cpp` — без изменений.

**AC:**
- [ ] Создан CMake-таргет `assimp_importer` (static library) — единственный таргет, линкующий Assimp
- [ ] `scene/CMakeLists.txt` не содержит `PRIVATE assimp` — зависимость убрана
- [ ] `editor_system/CMakeLists.txt` линкует `assimp_importer` вместо `assimp` напрямую
- [ ] `scn_model_importer_adapter` и `scn_assimp_resource_system_wrapper` перемещены в `assimp_importer/`
- [ ] Создан `scn_assimp_scene_parser` с `parse_result parse_assimp_scene()` — единая точка парсинга
- [ ] `texture_store_interface` абстрагирует разницу между runtime (res://) и editor (filesystem) текстурами
- [ ] Дублированный код (`convert_to_glm`, `decompose_aimatrix`, `store_data`, `process_mesh_geometry`, `build_mesh_skin_weights`, `build_prefab_node`, `find_material_texture`, `process_material`) существует только в `assimp_importer/`, не в editor и не в scene
- [ ] `edt_model_importer.cpp` уменьшен до ~100-150 строк (I/O + async split + вызов `parse_assimp_scene`)
- [ ] `scn_model_importer_adapter.cpp` уменьшен до ~50-80 строк (I/O + вызов `parse_assimp_scene` + finalize)
- [ ] CMake-опция `ENABLE_ASSIMP_IMPORTER=OFF` → проект собирается без Assimp, адаптер не регистрируется
- [ ] CMake-опция `ENABLE_ASSIMP_IMPORTER=ON` (default) → всё работает как раньше
- [ ] Импорт `.glb` через editor работает как раньше
- [ ] Импорт `.glb` через `res::require<>()` в runtime работает как раньше

## Порядок выполнения

```
US-34-8 (CMake-модуль assimp_importer + unified parser)
  ├── US-34-1 (shared helpers — уже часть US-34-8)
  ├── US-34-2 (shared prefab node builder — уже часть US-34-8)
  │     └── US-34-6 (камеры и свет)
  ├── US-34-3 (PBR текстуры)
  ├── US-34-4 (layout по всем мешам)
  └── US-34-7 (тесты)
US-34-5 (import options UI) — параллельно с US-34-8
```

## Риски

- **Circular dependency.** `assimp_importer` зависит от `scene` (для типов desc) и `render` (для `geometry_desc`). Нужно убедиться что `scene` не зависит обратно от `assimp_importer`. Регистрация адаптера — через `game_init` или условный `#if HAS_ASSIMP_IMPORTER`, не через прямую зависимость.
- **Совместимость шейдеров.** Новые PBR sampler slots (metalness, roughness, AO) требуют поддержки в фрагментных шейдерах. Без обновления шейдеров новые текстуры не повлияют на рендеринг.
- **Обратная совместимость material_desc.** Новые поля (дополнительные samplers, defines) должны игнорироваться старыми шейдерами.
- **`#if HAS_ASSIMP_IMPORTER` в game_init.** Требует условной компиляции в engine-слое. Альтернатива: регистрация адаптера через модульную систему (assimp_importer как отдельный `module_interface`).

## Критерии завершения эпика

- [ ] Весь Assimp-зависимый код изолирован в `assimp_importer` — ни `scene`, ни `editor_system` не линкуют Assimp напрямую
- [ ] Проект собирается с `ENABLE_ASSIMP_IMPORTER=OFF` без ошибок линковки
- [ ] Нет дублированного кода между `edt_model_importer` и `scn_model_importer_adapter`
- [ ] PBR-модель (glTF с metalness/roughness) импортируется с корректными текстурными слотами
- [ ] Layout строится по всем мешам, а не по первому
- [ ] Диалог Import Options доступен перед импортом
- [ ] Камеры и свет импортируются (если включена опция)
- [ ] Юнит-тесты на shared helpers проходят
