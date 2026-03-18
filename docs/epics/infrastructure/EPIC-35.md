# EPIC-35: Engine Model Importer Adapter — модернизация для runtime/mods

**Status:** planned
**Theme:** infrastructure
**Dependencies:** EPIC-34

---

## Мотивация

`scn_model_importer_adapter.cpp` (598 строк) — runtime-адаптер ресурсной системы, позволяющий загружать `.glb`/`.fbx`/`.obj`/`.gltf` напрямую через `res::require<>()`. Он нужен для:

- **Мод-поддержки:** моддеры могут положить `.glb` в ресурсную папку и использовать без предварительного импорта через редактор.
- **Быстрой разработки:** загрузка моделей «как есть» без шага export-to-project.
- **Runtime загрузки:** DLC или пользовательский контент, доступный через VFS.

Текущие проблемы:

1. **~400 строк дублируются с `edt_model_importer.cpp`.** После EPIC-34 (US-34-1, US-34-2) общий код будет в `scn_assimp_helpers` — адаптер нужно мигрировать на него.

2. **Не потокобезопасен.** `operator()` вызывается ресурсной системой в произвольном потоке. Внутри — `desc_system.create_instance()` и `skinning_manager.register_weights()`, которые могут быть не thread-safe.

3. **Нет обработки ошибок.** При ошибке Assimp возвращается пустой `shared_ptr` без диагностики для вызывающего кода. Логируется, но вызвавший код не знает причину.

4. **Текстурный I/O через resource_system.** Для внешних текстур (рядом с моделью) адаптер использует `fetch_data(tag)` — работает только если текстуры уже зарегистрированы в VFS. Если текстура лежит рядом с `.glb`, но не в `res://` — она не найдётся.

5. **Нет `import_options`.** В отличие от editor, нет способа передать масштаб, флаги Assimp, настройки.

6. **Материалы инлайнятся в prefab JSON.** Editor генерирует отдельные `material_desc` файлы, а engine adapter инлайнит материал прямо в `mesh_node_desc`. Это мешает переиспользованию и горячей замене.

## User Stories

### US-35-1: Миграция на scn_assimp_helpers
**Файлы:** `core/engine/scene/adapters/scn_model_importer_adapter.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.h`
**Зависимости:** EPIC-34 US-34-1, US-34-2

Заменить все локальные дублированные функции на вызовы из `scn::assimp_helpers`. Адаптер должен стать тонкой обёрткой: I/O + material_processor callback + передача результата в resource_system.

**AC:**
- [ ] Все `convert_to_glm`, `decompose_aimatrix`, `store_data`, `process_mesh_geometry`, `build_mesh_skin_weights` удалены из адаптера — используются из `scn_assimp_helpers`
- [ ] `build_prefab_node` вызывается из helpers с callback для материалов
- [ ] `scan_bones`, `build_keyframes_map`, `build_animations_desc` — из helpers
- [ ] Адаптер уменьшился до ~150-200 строк (I/O + glue code)
- [ ] Импорт `.glb` через `res::require<>()` работает как раньше

### US-35-2: Выделить материалы в отдельные memory:// ресурсы
**Файлы:** `core/engine/scene/adapters/scn_model_importer_adapter.cpp`
**Зависимости:** US-35-1

Сейчас engine adapter инлайнит `material_desc` JSON прямо в `mesh_node_desc::material`. Editor-импортер создаёт отдельные `memory://name/materials/Material.mat.desc`. Нужно унифицировать — engine adapter тоже должен создавать отдельные desc-ресурсы.

**AC:**
- [ ] Материалы сохраняются как `memory://{prefix}/materials/{name}.mat.desc`
- [ ] `mesh_node_desc::material` ссылается на тег, а не содержит инлайн-JSON
- [ ] PBR-карты (из US-34-3) корректно импортируются и в engine adapter
- [ ] Текстурные desc тоже создаются как отдельные ресурсы (как в editor)

### US-35-3: Улучшить обработку ошибок
**Файлы:** `core/engine/scene/adapters/scn_model_importer_adapter.h`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`

Сейчас при ошибке возвращается `nullptr` — вызывающий код не знает причину. Нужна диагностика.

**AC:**
- [ ] При ошибке Assimp логируется `egLOG_ERR` с описанием
- [ ] При отсутствии мешей — warning, не ошибка (некоторые модели содержат только скелет)
- [ ] При отсутствии текстур рядом с моделью — warning с путём, fallback на дефолтную текстуру (не крэш)
- [ ] Если `desc_system.create_instance` не удался — ошибка логируется, возвращается `nullptr` с внятным сообщением

### US-35-4: Thread-safety аудит
**Файлы:** `core/engine/scene/adapters/scn_model_importer_adapter.cpp`, `core/engine/scene/scn_ecs_assembler.cpp`, `core/engine/render/render_system/skinning/rnd_skinning_manager.h`

`operator()` может вызываться из фонового потока ресурсной системы. Нужно убедиться, что все операции безопасны.

**AC:**
- [ ] Аудит: `res::get_system().store()` — thread-safe (уже mutex-protected) — подтвердить
- [ ] Аудит: `desc_system.create_instance()` — если не thread-safe, перенести на main thread (аналогично editor)
- [ ] Аудит: `skinning_manager.register_weights()` — если не thread-safe, защитить mutex или перенести
- [ ] Документировать потокобезопасность в комментарии к `operator()`
- [ ] Если нужна двухфазная загрузка (background + main) — реализовать по аналогии с editor

### US-35-5: Поддержка текстур рядом с моделью через VFS
**Файлы:** `core/engine/scene/adapters/scn_assimp_resource_system_wrapper.h`, `core/engine/scene/adapters/scn_assimp_resource_system_wrapper.cpp`, `core/engine/scene/adapters/scn_model_importer_adapter.cpp`

Если `.glb` лежит в `res://models/robot.glb`, а текстура `diffuse.png` рядом (`res://models/diffuse.png`), текущий wrapper может не найти её. Нужно улучшить resolution.

**AC:**
- [ ] `engine_assimp_resource_system_wrapper` резолвит относительные пути текстур от директории модели
- [ ] Если `res://models/robot.glb` ссылается на `diffuse.png`, ищется `res://models/diffuse.png`
- [ ] Если текстура не найдена через VFS — логируется warning, используется fallback
- [ ] Embedded текстуры продолжают работать как раньше

## Порядок выполнения

```
US-35-1 (миграция на helpers) — блокирован EPIC-34 US-34-1/34-2
  ├── US-35-2 (материалы как отдельные ресурсы)
  └── US-35-5 (VFS текстуры)
US-35-3 (обработка ошибок) — параллельно с US-35-1
US-35-4 (thread-safety) — после US-35-1
```

## Риски

- **EPIC-34 — блокирующая зависимость.** US-35-1 невозможен без готовых `scn_assimp_helpers`. Можно начать с US-35-3 и US-35-4 параллельно.
- **Thread-safety может потребовать изменений в desc_system и skinning_manager.** Если эти системы не thread-safe, рефакторинг выйдет за рамки этого эпика.
- **VFS resolution для текстур.** Текущая VFS-система может не поддерживать relative path resolution. Возможно потребуется расширение `res::tag` или `resource_system`.

## Критерии завершения эпика

- [ ] `scn_model_importer_adapter.cpp` не содержит дублированного кода с `edt_model_importer.cpp`
- [ ] Адаптер уменьшен до ~150-200 строк (тонкая обёртка)
- [ ] Материалы создаются как отдельные `memory://` ресурсы (не инлайн)
- [ ] Ошибки логируются информативно, нет тихих `nullptr` возвратов
- [ ] Thread-safety задокументирована и обеспечена
- [ ] `.glb` модели с внешними текстурами загружаются корректно через `res://`
