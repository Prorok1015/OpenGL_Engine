# Дизайн: форматы данных v2 — node_id, overrides, __version, __guid

**Issue:** #7 (stories #63, #85, #86, #87, #88; баги #64, #65) · **Статус:** утверждён владельцем 2026-10-17 (все рекомендации §8 приняты) · **Дата:** 2026-10-17

## 0. Что лежит в репозитории
- В git **15 `.desc`** (после удаления дубля) (а не ~50): `Editor/res/assets/`, `objects/`, `Editor/sandbox/` в `.gitignore`; сгенерированные импортёром `*.prefab.desc` / `*.geom.desc` есть только локально у владельца → нужна **миграция при загрузке**, а не только разовый скрипт.
- Уровни `level_desc` (6): `levels/{editor_level, editor_level2, test_level, smoke_level, prefab_level, backpack_level}`; шаблоны (3): `templates/{level, world, prefab_preview}`; материалы/текстуры/геометрия/скайбокс (6) — им нужен только `__version`.
- Мусорные узлы: в `test_level` (бывш. `my_level123`; дубль `my_level_3` удалён) узел `backpack.obj` с ~80 пустыми детьми без компонентов; в `editor_level2` — `simple/ROOT`, `robot hot reload/ROOT` без компонентов (ссылки на модель потеряны — след старого пути сохранения, родственного #82).

## 1. Как устроено сейчас
- `prefab_node` (`core/engine/scene/level/scn_prefab_desc.h:22-38`): имя, `vec3` pos/rot/scale с умолчаниями 0/0/1, `unordered_map` компонентов, вектор детей. В JSON `children` — объект с ключом-именем (`scn_prefab_desc.cpp:88-92`, `178-184`) → **имя = идентичность**, одноимённые соседи невыразимы. Короткая форма «узел = компонент» читается в `:72, :94`, сворачивается при записи в `:145-155`.
- Инстанс префаба (а) — строковая ссылка компонента на `*.prefab.desc` → структурный спавнер (`core/engine/game_system/gs_game_init.cpp:49-57`) → `assemble_prefab`; переопределить детей нельзя. Подозрение: `assemble_prefab` перезаписывает `local_transform` трансформом корня префаба (`scn_prefab_desc.cpp:528`) после `spawn_child` (`:271`).
- Инстанс (б) — компонент `{"__type":"prefab_desc","__parent":X}` (`:204-217`), только в тестах; дети сопоставляются по имени (`:246-311`).
- Файловый `__parent`: `desc_adapter_t.cpp:36-68` (`copy_to` + `deserialize`); слой префаба над родителем (`scn_prefab_desc.cpp:102-121`) заменяет одноимённого ребёнка целиком.
- **#64 (sentinel):** признак «поле задано» теряется при разборе (`:76-78`) и записи (`:160-165`); слияние сравнивает с 0/1 в трёх копиях: `:258-266`, `:408-416`, `:438-446`.
- **#65 (shallow):** `:225-231`, `:348-352` — `final_overrides[key] = value` только по верхним ключам.
- Hot reload сопоставляет детей по имени (`:367-396`), сборка задублирована (~120 строк).
- Редактор работает по именам: `find_node_by_name`/`remove_node_by_name` (`Editor/code/editor_system/controller/edt_scene_editor.cpp:255-290`); `sync_ecs_transforms_to_desc` строит плоскую карту имён по всему дереву (`:205-236`); инспектор правит `comp.overrides` напрямую (`edt_inspector_panel.cpp:106-119`).
- Импортёры: `unique_child_name` только для мешей, рекурсивные дети схлопываются при совпадении имён (`edt_model_importer.cpp:496`, `scn_model_importer_adapter.cpp:493`).
- Сохранение уровня в обход адаптера: `edt_level_controller.cpp:296-307` (`level_desc::build_json` + `json::serialize`); импортёр — `serialize_to_bytes` (`edt_model_importer.cpp:712`) → служебные поля проставлять **одной функцией** из всех мест.

## 2. node_id
- C++: `uint64_t node_id` (0 — невалидный). JSON: строка 16 hex, ключ **`__id`** (ADR-05); строка, т.к. числа > 2^53 ломают внешние JSON-инструменты.
- Уникальность — в пределах одного desc-файла. Дубликат при загрузке перегенерируется у второго узла с предупреждением.
- Генерация: создание в редакторе — случайный (`desc::new_node_id()`, `mt19937_64`); дублирование — новые id поддерева (записи override внутри инстанса не трогаются); reparent/rename сохраняют id; импортёр — детерминированно `fnv1a_64(путь имён с индексом дубля)` (`core/core/desc/desc_system.h:19`), напр. `ROOT/Armature/geometry_0#1`; старые файлы без `__id` — тот же хеш пути. Сопоставление со старым файлом и `.meta` — EPIC-62.
- Адресация в инстансе — путь id от корня префаба: `["c41f…"]`; вложенный — `["<id B в A>", "<id в B>"]`; `[]` — корень.
- В мире: `scn::node_id_component{ path }` + `reg.ctx()` → `scn::node_index` (`hash_combine(path) → entity`, `find(span<const u64>)`), заполняется общей `reconcile(base, records, live)`, которая заменяет обе копии сборки (`:195-311`, `:314-496`). Редактор и hot reload ищут по ней.
- Имена анимационных каналов остаются именами (контракт с glTF).

## 3. Записи override
Дети — **массив** `[{ "__id", "name", … }]`. Короткая форма при записи не используется (читается миграцией).

До (`prefab_level.desc`, сжато):
```json
"scene":{"children":{"solder.prefab":{"transform":{"rotation":[-90,0,0]},
  "components":{"solder.prefab":"res://assets/solder/solder.prefab.desc"}}}}
```
После (v2):
```json
"scene":{"__id":"3a9f…","name":"scene","children":[
 {"__id":"e1b2…","name":"solder.prefab","transform":{"rotation":[-90,0,0]},
  "components":{"solder.prefab":"res://assets/solder/solder.prefab.desc"},
  "overrides":[
   {"target":["c41f…"],"component":"transform","path":"/position","value":[0,0,0]},
   {"target":["c41f…"],"component":"stat","path":"/limits/max","value":20},
   {"target":["9d02…"],"op":"remove","component":"stat"},
   {"target":["77aa…"],"op":"remove"},
   {"target":[],"op":"add_node","value":{"__id":"5b1e…","name":"Lamp","components":{}}}]}]}
```
- Поля: `target` (путь id), `component` (ключ или псевдокомпонент `transform`), `path` (RFC 6901), `value`, `op` = `set` (умолч.) | `remove` | `add_node`. `remove` без `component` удаляет поддерево; `set` с `path:""` и объектом с `__type` добавляет компонент.
- **Присутствие:** нет записи — наследуется; есть — override, включая 0/0/1 → #64 закрывается (sentinel-сравнения исчезают во всех трёх местах).
- **Глубокое слияние (#65):** эффективный JSON = `serialize(parent_desc)` ⊕ `base.overrides`, затем записи `set` по pointer'у → `deserialize` нового экземпляра (`core/engine/scene/scn_ecs_assembler.cpp:58-70`). Объекты — рекурсивно; массивы и скаляры — целиком.
- Порядок — по массиву; при сохранении канонизация: сортировка `(target, component, path)`, повторы схлопываются, запись с pointer'ом P поглощает более ранние записи потомков P; компоненты — в отсортированном порядке.
- Файловый `__parent` префаба — те же `overrides` на корне относительно корня родителя.
- Инспектор: commit пишет запись, «сброс к базе» удаляет её (Apply/Revert — EPIC-25).
- Уточнения шага 5 (`prefab_desc`/`world_desc` v3): записи лежат в `prefab_node::overrides` и применяются к копии содержимого префаба при `reconcile`; запись, чей `target` проходит через вложенный инстанс, передаётся этому инстансу. `set` с `path:""` добавляет компонент или сливает объект в существующий (порядок с pointer-записями того же компонента сохраняется при канонизации, такая запись ничего не поглощает); `add_node` с уже существующим `__id` сливается в этот узел (так мигрирует слой v2). Миграция v2→v3: слой явного вложенного инстанса (`{"__type":"prefab_desc","__parent":X}` + остальные компоненты и дети) превращается в записи. Собственные компоненты и дети узла-инстанса — дополнения к содержимому префаба. Канонизация консервативна: схлопываются и сортируются только `set` по «простым» pointer'ам (не пустой, без индексов массива и `-`), и только если между записями нет `remove`/`add_node` связанного узла; остальное остаётся в записанном порядке (`add_node` с существующим `__id` — слияние, поэтому он не гасится `remove`). Положение инстанса: если у узла есть `transform` (`prefab_node::has_transform`, пустой объект тоже), он задаёт положение; иначе — корень префаба с записями `target:[]` `transform`.

## 4. __version и миграции
- Версия на **каждом объекте с `__type`** (inline-desc уходят в `memory://` и снова проходят адаптер). Отсутствие = v1.
- API в `desc::desc_system`: `register_migration(type, int from, std::function<void(json::object&)>)`, `current_version(type)`, `migrate_tree(json::object&, const res::tag&)`; регистрация рядом с `register_desc` (`gs_game_init.cpp:30`).
- Вызов: `desc_adapter_t::deserialize` до `create_instance` (`desc_adapter_t.cpp:30`), обход в глубину, массивы чисел пропускаются (`geom.desc` ~10 МБ).
- Запись: `desc::stamp_header(json&)` (`__version`, сохраняет `__guid`) — в `desc_adapter_t::serialize`, `level_desc::build_json` (`scn_level_desc.cpp:25`) и импортёрах. Ключи `__*` исключаются из `overrides` в `parse_comp` (`scn_prefab_desc.cpp:61-64`).
- В эпике `prefab_desc`/`world_desc`/`level_desc` v1→v2 (дети массивом, `__id` из хеша пути, явный `__type`).
- Файл новее поддерживаемого — ошибка загрузки (`version N > supported M`); редактор такой файл не открывает и не перезапишет. Явная миграция проекта (EPIC-58) строится поверх `migrate_tree`.

## 5. __guid
- UUID v4 строкой (подойдёт и для `.meta` в EPIC-57). Только в корне файла `res://`; у `memory://` и inline — нет.
- Поле `guid` в `desc_base`, **не копируется** в `operator=`/`copy_to` (как `m_tag`, `core/core/desc/desc_base.hpp:32-37`).
- Генерируется при первом сохранении; редактор держит guid уровня в `shared_state`.
- Не делаем сейчас: asset database, ссылки `{guid, path}`, `.meta`; `res::tag` остаётся ключом рантайма.

## 6. Миграция и проверка
- CLI `tools/desc_resave` на движковых библиотеках (load с миграцией → запись с отступами) — зародыш команды EPIC-58.
- Тест `tests/desc_repo_roundtrip_tests.cpp` по `Editor/res/**/*.desc`: load без ошибок; `save(load(f)) == f` как `json::value`; идемпотентность; текущий `__version` у каждого объекта с `__type`.
- Визуально: `tools/run_headless.sh` по 7 уровням до/после.
- Коммиты (каждый CI-зелёный): 1) реестр миграций + `__version` + `stamp_header` (#86); 2) `__guid` (#87); 3) `node_id`, `node_index`, импортёры с дублями имён (#63); 4) миграция v1→v2 + общая `reconcile`; 5) записи override + deep merge, снятие `// BUG:` (#85, закрывает #64, #65); 6) редактор по id; 7) `desc_resave` по `Editor/res` + тест по репозиторию (#88).
- #82 влить до шага 4; на шаге 6 его тест переписывается в v2.

## 7. Риски
- `serialize(parent)` на каждый компонент с записями — кэш эффективного JSON на `(prefab, component)`.
- Совпадение хеша пути у реальных дублей — индекс `#n`.
- Локальные `prefab.desc` владельца мигрируют только при загрузке до явной команды EPIC-58.
- Перезапись трансформа корнем префаба (`:528`) — проверить до шага 5.

## 8. Решения владельца (2026-10-17: «всё по рекомендации»)
1. Ключ id в JSON: `__id` (ADR-05) или `node_id`? → **принято:** `__id` в JSON, `node_id` в C++.
2. Мусорные узлы (`backpack.obj` с пустыми детьми; `simple`/`robot hot reload` с `ROOT`) → **принято:** удалить отдельным коммитом до миграции; `my_level_3` удалён как дубль, `my_level123` переименован в `test_level` (решение владельца 2026-10-17).
3. Форма (б) и префаб-варианты через `__parent` есть только в тестах → **принято:** не мигрировать автоматически их детей-по-имени, тесты переписать в v2, на такой форме — предупреждение.
4. Перезаписать все `.desc` с отступами (включая однострочные уровни) → **принято:** да, и редактор сохраняет так же.
5. `__guid` для `memory://` и сгенерированных префабов → **принято:** не выдавать до `.meta` (EPIC-57).
