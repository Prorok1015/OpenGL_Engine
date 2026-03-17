# EPIC-22: Asset Import Pipeline — Импорт 3D-моделей в проект

**Theme:** editor
**Status:** in_progress
**Depends on:** EPIC-12 (Model Importer → Prefab Output), EPIC-03 (Asset Browser)

---

## Цель

Полноценный pipeline импорта 3D-моделей: пользователь выбирает внешний файл (glb/fbx/obj/gltf) из произвольного места на диске, редактор загружает его напрямую из файловой системы (не через VFS), конвертирует в prefab_desc, и записывает все ресурсы на диск в ресурсную папку проекта с корректными `res://` путями.

---

## Проблема текущей архитектуры

Сейчас `show_file_dialog()` делает:
```
selected_path.lexically_relative(base_path) → res::tag::make(relative)
→ res://../../Models/robot.glb  ← невалидный тег, вылезает за res/
```

Файл загружается через VFS (`res://`) — но внешний файл **не является ресурсом проекта**. Это работает случайно через `../` и ломается при переносе проекта.

**Правильный подход**: внешний файл читается напрямую из FS → конвертируется → результат сохраняется в `res://` как настоящий ресурс.

---

## Архитектура: два этапа

```
[Внешний файл]                          [Ресурсная папка проекта]
C:/Models/robot.glb
C:/Models/diffuse.png
        │
        ▼ Этап 1: Import
   model_importer
   (читает напрямую из FS)
        │
        ▼
   import_result {
     prefab (в памяти)
     memory://robot/robot.prefab.desc
     memory://robot/robot.geom.desc
     memory://robot/tex/diffuse.txm.desc
     memory://robot/tex/diffuse.png
   }
        │
        ▼ Этап 2: Export to Project
   asset_exporter
   (remapping memory:// → res://)
        │
        ▼
   res://objects/robot/robot.prefab.desc
   res://objects/robot/robot.geom.desc
   res://objects/robot/tex/diffuse.txm.desc
   res://objects/robot/tex/diffuse.png
```

---

## Структура файлов на диске (целевой результат)

```
res/objects/robot/
├── robot.prefab.desc          ← prefab JSON, все ссылки res://
├── robot.geom.desc            ← геометрия (сериализованный JSON)
├── materials/
│   ├── Material.0.mat.desc    ← material_desc
│   └── Material.1.mat.desc
├── textures/
│   ├── diffuse.txm.desc       ← texture_2d_desc (ссылается на .png)
│   ├── diffuse.png             ← бинарный файл текстуры
│   └── normal.png
└── source/                     ← опционально
    └── robot.glb               ← копия оригинала
```

---

## User Stories

### US-22-1 — `edt::model_importer` сервис ✅

**Задача:** выделить логику загрузки модели из `model_importer_adapter` в отдельный сервис редактора, работающий с абсолютными путями файловой системы (не через VFS).

**Новый класс:**
```cpp
namespace edt {
    struct import_result {
        std::shared_ptr<scn::prefab_desc> prefab;
        res::tag                          root_tag;    // memory://name/name.prefab.desc
        std::vector<res::tag>             all_tags;    // всё что было store() в memory://
        std::filesystem::path             source_path; // оригинальный файл
    };

    class model_importer {
    public:
        model_importer(desc::desc_system& ds);

        // Загружает модель по абсолютному пути FS, результат — в memory://
        import_result import(const std::filesystem::path& abs_path);
    private:
        desc::desc_system& m_desc;
    };
}
```

**Ключевые отличия от текущего adapter'а:**
- Принимает `std::filesystem::path`, не `res::tag`
- Assimp читает файл **напрямую** из FS (`ReadFile(abs_path)`)
- IO wrapper привязан к **директории модели** (для разрешения относительных текстур рядом с `.glb`), а не к ресурсной папке
- Результат складывается в `memory://` с чистыми именами
- Возвращает `import_result` с полным списком всех созданных `memory://` тегов — manifest не нужно строить post-factum
- Регистрирует skinning weights в skinning_manager (как сейчас)

**Assimp IO для внешних файлов:**
```cpp
class filesystem_assimp_io : public Assimp::IOSystem {
    std::filesystem::path m_base_dir;     // директория модели
    const std::vector<std::byte>& m_root; // байты основного файла (уже прочитаны)
    std::filesystem::path m_root_path;    // путь основного файла

    Assimp::IOStream* Open(const char* pFile, const char* pMode) override {
        if (pFile == m_root_path) return MemoryIOStream(m_root);
        // Подчинённые файлы — читаем из директории модели
        auto full = m_base_dir / pFile;
        auto bytes = read_file_from_disk(full);
        return MemoryIOStream(bytes);
    }
};
```

**Затронутые файлы:**
- Новый `Editor/code/editor_system/edt_model_importer.h/.cpp`
- Новый `Editor/code/editor_system/edt_filesystem_assimp_io.h/.cpp`
- Переиспользует: `process_material()`, `process_mesh_geometry()`, `build_prefab_node()`, `build_mesh_skin_weights()` из `scn_model_importer_adapter.cpp` (вынести общие функции в shared header или дублировать в редакторе)

**Вопрос**: `model_importer_adapter` в core остаётся для загрузки уже импортированных моделей из `res://` (например `res://objects/robot/robot.prefab.desc`). Или его можно удалить если все модели будут импортированы в `.desc` формат?

---

### US-22-2 — Import Dialog UX ✅

**Изменения в `editor_system`:**
- `show_file_dialog()` переписывается: возвращает `std::filesystem::path` (абсолютный), не создаёт `res::tag`
- После выбора файла — вызывает `model_importer::import(abs_path)`
- При успешном импорте — открывает Export Dialog (US-22-3)
- Пункт меню: **File → Import Model...**

```cpp
// Было:
auto relative = file_dialog.get_selected_path().lexically_relative(base_path);
res::tag tag = res::tag::make(relative.string());
m_res.require<scn::prefab_desc>(tag);

// Стало:
auto abs_path = file_dialog.get_selected_path();
auto result = m_model_importer.import(abs_path);
m_pending_import = std::move(result);
m_export_dialog_open = true;
```

**Затронутые файлы:** `edt_editor_system.h/.cpp`, `edt_dockspace.cpp`

---

### US-22-3 — Export to Project Dialog ✅

**ImGui-модал** `edt_asset_export_dialog`:

```
┌─────────────────────────────────────────────────────────┐
│  Export to Project                                      │
├─────────────────────────────────────────────────────────┤
│  Asset name: [robot              ]                      │
│                                                         │
│  Target folder:  objects/robot/        [Browse...]      │
│                                                         │
│  Files to be created:                                   │
│    robot.prefab.desc                                    │
│    robot.geom.desc                                      │
│    materials/Material.0.mat.desc                        │
│    textures/diffuse.txm.desc                            │
│    textures/diffuse.png                                 │
│  [ ] Copy source file (robot.glb → source/)             │
│                                                         │
│              [Cancel]   [Export]                        │
└─────────────────────────────────────────────────────────┘
```

**Поведение:**
- Получает `import_result` — `all_tags` уже содержит полный список ресурсов
- "Browse..." — `edt_file_dialog` в режиме `FOLDERS_ONLY`, base_path = `res/`
- Default target: `objects/<model_name>/`
- Asset name → переименовывает root `.prefab.desc`
- Если файл существует → пометка `[overwrite]`
- Export → вызывает `asset_exporter::export_to_project()`

**Затронутые файлы:** новый `edt_asset_export_dialog.h/.cpp`

---

### US-22-4 — Asset Exporter (запись на диск + remapping) ✅

**Задача:** записать все `memory://` ресурсы из `import_result` на диск с remapping путей.

```cpp
namespace edt {
    class asset_exporter {
    public:
        asset_exporter(res::resource_system& res);

        // Строит mapping: memory://tag → res://target
        std::unordered_map<res::tag, res::tag> build_remap(
            const import_result& result,
            const std::string& asset_name,
            const std::string& target_folder);   // e.g. "objects/robot"

        // Записывает всё на диск
        bool export_to_project(
            const import_result& result,
            const std::unordered_map<res::tag, res::tag>& remap);

    private:
        res::resource_system& m_res;
    };
}
```

**Алгоритм `export_to_project`:**
```
for each (mem_tag, res_tag) in remap:
  1. bytes = res::get_system().fetch_data(mem_tag)  // читаем из memory://
  2. if .desc file:
       parse JSON → walk all strings → replace memory:// with res:// using remap
       bytes = serialize back
  3. abs_path = get_resources_path() / res_tag.relative()
  4. create_directories(abs_path.parent_path())
  5. write bytes to abs_path via ofstream
  6. res::get_system().store(res_tag, bytes)  // регистрируем в VFS
```

**Порядок записи:** листья первыми (текстуры → материалы → геометрия → prefab).

**Затронутые файлы:** новый `edt_asset_exporter.h/.cpp`

---

### US-22-5 — Интеграция + Asset Browser refresh

**После экспорта:**
1. `imported_models_list` обновляется с новым `res://` тегом
2. Asset Browser перечитывает директорию → показывает новые файлы
3. Модель доступна для добавления в сцену по `res://` тегу
4. `memory://` ресурсы можно очистить (или оставить как кэш)

**Затронутые файлы:** `edt_editor_system.cpp`, `edt_asset_browser_panel.cpp`

---

### US-22-6 — Копирование source-файла (опционально, отложена)

Чекбокс "Copy source file" → `std::filesystem::copy_file(source, target/source/name.ext)`.

---

## Новые файлы

```
Editor/code/editor_system/
├── edt_model_importer.h/.cpp          # US-22-1: загрузка модели из FS
├── edt_filesystem_assimp_io.h/.cpp    # US-22-1: Assimp IO для внешних файлов
├── edt_asset_exporter.h/.cpp          # US-22-4: запись на диск + remapping
└── edt_asset_export_dialog.h/.cpp     # US-22-3: ImGui диалог экспорта
```

---

## Технические решения

| Проблема | Решение |
|---|---|
| Внешние файлы загружались через `res://` с `../../` | `model_importer` работает с абсолютными путями FS |
| Assimp не находил текстуры рядом с моделью | `filesystem_assimp_io` привязан к директории модели |
| Не знали какие memory:// ресурсы создались | `import_result.all_tags` — адаптер сам трекает |
| Чтение memory:// байт обратно | `res::get_system().fetch_data(tag)` — уже есть |
| Remapping memory:// → res:// | JSON-walk в `asset_exporter` при записи |
| Параллельные импорты | Каждый `import_result` изолирован, нет глобального состояния |

---

## Фазы реализации

| Фаза | US | Результат |
|---|---|---|
| **1** | 22-1 | `model_importer` загружает модели из FS, возвращает `import_result` |
| **2** | 22-2, 22-3 | Диалоги: выбор файла → выбор папки назначения |
| **3** | 22-4, 22-5 | Экспорт на диск, re-registration, Asset Browser update |
| **4** | 22-6 | Копирование source-файла |

---

## Вопрос к обсуждению

**`model_importer_adapter` в core** — оставить или удалить?
- *Оставить*: нужен если `.glb` файлы лежат внутри `res/` и загружаются через pipeline
- *Удалить*: если все модели импортируются через редактор в `.desc` формат, адаптер не нужен — загрузка идёт через `desc_adapter`
- **Рекомендация**: оставить на данном этапе, удалить позже когда все модели перейдут на `.desc`
