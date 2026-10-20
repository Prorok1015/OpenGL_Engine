# Дизайн: схема desc — type_schema, generic-операции, инспектор, компоненты без C++-типа (эпик #16)

**Issue:** #16 (stories #128–#131) · **Решение владельца (финальное):** Boost.Describe по ADR-04: `describe` и `mp11` добавляются в `BOOST_INCLUDE_LIBRARIES` и в SDK-замыкание (STAGE_D_PLAN §5.6) · **Новое требование:** DISTRIBUTION.md §7.1: компоненты из wasm-модулей описываются схемой, а не C++-типом; id компонента — по имени · Основа: `CHIMERA_ROADMAP.md` ADR-04, ADR-06; `ARCHITECTURE_REVIEW.md` §3.7; `DATA_FORMATS_V2.md` §3–§6; `STAGE_D_PLAN.md` §1 #16, §2 (волна 1 C, волна 2); `HEADER_SPLIT.md` (desc переезжает в PR-2) · **Статус:** утверждён владельцем 2026-10-18 (решения в конце)

## 1. Как устроено сейчас (aibranch)

- **Контракт desc.** `desc::desc_base` (`core/core/desc/include/snk/desc/desc_base.hpp:29-31`) содержит три чистых виртуальных метода: `copy_to`, `deserialize(desc_system&, const json::object&)`, `serialize(json::object&)`. Метаданных полей нет. `desc_hints.hpp:4-17` (`range`, `color`, `filter`, `hint_value`) нигде не используется.
- **Типы.** 21 desc-тип, все рукописные. Сцена — 17 (`scn_registration_scope.cpp:91-107`): material, prefab, world, level, anchor, camera*, directional_light*, skybox*, mesh_node, object, bone, skinning, skin_weights, animation_controller, animated_node, animation_clip, animation_collection (* — `register_component_desc`, их можно добавить в редакторе). Render — 4 (`rnd_render_service_init.cpp:22-25`): geometry, texture, texture_2d, texture_cubemap. Объём `.cpp`: от 7 строк (anchor) до 1053 (prefab).
- **copy_to = `static_cast`.** Так устроены все 21 тип (напр. `scn_directional_light_desc.h:18-21`, `scn_camera_desc.h:20-23`). Вызов с объектом другого типа даёт UB. Такой вызов возможен в двух местах: файловый `__parent` (`desc_adapter_t.cpp:74`: тип родителя не проверяется) и спавн компонента поверх родителя (`scn_ecs_assembler.cpp:66`) (§3.7).
- **serialize/deserialize — вручную, у каждого типа свои правила.** Свет: читает только присутствующие ключи и оставляет умолчание, если значение битое (`scn_directional_light_desc.cpp:8-24,26-32`); пишет всегда все 4 поля (`:34-40`). Камера: JSON-ключи отличаются от членов класса (`m_fov` → `"fov"`), позиция и поворот лежат во вложенном `transform` (`scn_camera_desc.cpp:34-39,67-70`), устаревший ключ `"texture"` читается как `color_targets[0]` (`:48-52`), теги пишутся только если валидны (`:78-83`). Skybox: ссылка на desc через `get_field_desc<T>` (`scn_skybox_desc.cpp:4-8`, `desc_system.h:175-190`), принимает inline-объект или тег.
- **enum ↔ string — таблицы, которые синхронизируют вручную.** `scn_material_desc.cpp:17-45` — копия render-таблицы, комментарий требует «byte-for-byte identical». `pass_queue` пишется строчными (`"opaque"`, `:62-79`), а enumerator — `OPAQUE`. В `rnd_texture_desc.cpp:7-140` шесть пар `*_to_string/_from_string`.
- **Наслоение и presence.** Эффективный JSON компонента = `serialize(parent)` ⊕ `overrides` (deep merge), затем `deserialize` в новый экземпляр (`scn_ecs_assembler.cpp:70-77`; `compose_json` в `scn_prefab_desc.cpp:570-590` применяет ещё и `edits` по pointer'у). **Presence хранится в JSON**: ключ есть в `prefab_comp_node::overrides` (`scn_prefab_desc.h:25-30`) или есть override-запись. В C++-структуре desc признака «задано» нет. `has_transform` — отдельный бит узла (`scn_prefab_desc.h:41`), он управляет записью пустого `transform` (`scn_prefab_desc.cpp:258`).
- **Override-записи v3.** `{target: [node_id…], component, path (RFC 6901), value, op}` (`scn_override_record.h:23-29`), канонизация (`scn_override_record.cpp:162-207`). Pointer'ы указывают в **сериализованный JSON** компонента, то есть JSON-ключи desc уже входят в формат данных. Операции над pointer'ами — в `desc_json_ops.h`.
- **Diff.** Есть только в редакторе: `override_edit::diff` (`edt_override_edit.cpp:197-211`). Сравнивает объекты по ключам, остальное — целиком, без знания типов. `write_component` превращает результат в одну запись на `(target, component)` (`:255-270`).
- **Инспектор.** Сначала ищется кастомный рендерер в `component_ui_registry` по `T::__type` (`edt_component_ui_registry.h:14-40`; есть для camera/light/skybox/skin/animation), иначе работает generic-JSON-редактор (`edt_inspector_panel.cpp:142-151,289-376`), он угадывает виджет по форме значения. Умолчания продублированы в рендерерах и уже расходятся с desc: камера в редакторе показывает `fov` 60 и `near` 0.01 (`edt_cr_camera.cpp:132-133`), а в desc `m_fov = 90`, `m_near_distance = 0.0001` (`scn_camera_desc.h:38-39`). Первая правка компонента без overrides копирует **все** поля родителя в `overrides` (`edt_inspector_panel.cpp:156-163`): presence «утекает», все поля начинают считаться переопределёнными.
- **Версии и миграции (ADR-06) уже есть** (#86): `register_migration/current_version/migrate_tree/stamp_header` (`desc_system.h:80-130,351-415`), вызов до `create_instance` (`desc_adapter_t.cpp:28`), регистрация через `registration_scope::migration` (`scn_registration_scope.h:43-47`). Текущие версии: prefab/world — v3, level — v2 (`scn_registration_scope.cpp:110-115`), остальные — v1. Тесты: `desc_migration_tests.cpp` (11), `scn_desc_roundtrip_tests.cpp` (37), `desc_repo_roundtrip_tests.cpp` (5, `save(load(f)) == f` по `Editor/res`). Инструмент `tools/desc_resave … --check` (`main.cpp:4-7,127-137`) в CI не запускается.
- **ECS.** `ecs::register_component<T>` (`ecs_component.h:35-41`) построен на `entt::meta<T>` и `type_hash<T>`, без C++-типа не работает. Boost: `BOOST_INCLUDE_LIBRARIES json asio program_options test` (`cmake/ThirdParty.cmake:89`). SDK кладёт Boost-заголовки линкуемых целей в `include/snk/boost` (`SnkInstall.cmake:81-101`).

## 2. Решение

**Главный принцип.** `type_schema` — **данные времени выполнения**, а не шаблон. Для C++-типов схема генерируется из Boost.Describe и таблицы атрибутов. wasm-модули и JSON-описания отдают ту же структуру в рантайме. Все generic-операции (serialize, deserialize, copy, equal, diff, инспектор, проверка override-записей) работают только с `type_schema` и не знают, откуда она взялась.

**Формат** (`core/core/desc/desc_schema.h`; после #109 — `include/snk/desc/`):

| Сущность | Поля |
|---|---|
| `type_schema` | `name` (стабильный, = `__type`), `id = fnv1a_64(name)` (`desc_system.h:28`), `version` (= `current_version`), `kind ∈ {desc, component, struct}`, флаги `is_component`/`is_document`, `fields[]`, `layout` (размер и выравнивание blob'а, только для runtime-схем), `ops` (§2.3) |
| `field_schema` | `name` (метка в UI), `key` — JSON-pointer относительно объекта (`"/fov"`, `"/transform/rotation"`), `kind`, `ref_type` (имя схемы для enum/struct/ссылки), `default` (`json::value`), `range{min,max,step}`, `write ∈ {always, if_valid, if_not_default, null_if_empty, never}` (`null_if_empty` — пустое значение пишется явным `null`; `never` — ключ только читается, см. «Расширение схемы для material/skybox»), `read_aliases[]` (`"/texture"`), флаги `optional`, `editor_hidden`, `read_only`, `color`, `degrees`, `tooltip`, `offset` (в blob) |
| `enum_schema` | `name`, `items[{string, int64}]`, `fallback` (значение для неизвестной строки, как `TRIANGLE` в `scn_material_desc.cpp:33`), `null_value` (элемент, который читается из `null` и пишется как `null` при `write = null_if_empty`) |
| `field_kind` | `bool, i32, u32, i64, u64, f32, f64, vec2/3/4, quat, color, string, enumeration, res_tag, desc_ref` (тег или inline-desc, как `get_field_desc`; в C++ — `res::res_handle<T>` с `.kind(field_kind::desc_ref)`, читается через `read_context`, пишется тегом), `node_ref` (node_id, hex-строка), `entity` (только runtime), `structure`, `array`, `custom` (собственный кодек типа, например `ds::fixed_vector<res::tag,4>`) |

**2.1. Генерация из C++ (Boost.Describe + таблица атрибутов).** Собственного макроса для списка полей нет: используем `BOOST_DESCRIBE_CLASS(T, (), (публичные), (), (приватные))` и `BOOST_DESCRIBE_ENUM` как есть. `desc_base` в список баз не входит: `type`/`guid` не поля схемы. Атрибуты задаются отдельной `constexpr`-таблицей по указателю на член:
`template<> inline constexpr auto desc::field_attrs<scn::camera_desc> = desc::attrs(desc::f(&camera_desc::m_fov).key("/fov").range(1,179), desc::f(&camera_desc::m_color_targets).alias("/texture").write_if_valid(), …);`
Поле без атрибутов получает `key = "/" + имя` и вид по C++-типу (`desc::kind_of<T>`). Умолчание берётся из сериализации `T{}`, поэтому дубли в редакторе исчезают (баг 60/90). `desc_hints.hpp` оживает: в нём живут строители атрибутов (`range`, `color`, `filter`, `hidden`, `read_only`, `degrees`). Для enum-имён, отличных от enumerator'ов (`OPAQUE` → `"opaque"`), задаётся `desc::enum_names<pass_queue>`. Название `DESC_FIELDS` из issue становится именем этого механизма, а не макроса (Q1).

**2.2. Runtime-схемы (wasm/JSON).** `desc::parse_type_schema(const json::object&)` читает те же поля, что в таблице (`{"name": "mymod.health", "version": 1, "fields": [{"name": "hp", "kind": "f32", "default": 100, "range": [0, 1000]}]}`). Раскладка blob'а считается по `kind` (выравнивание C, без указателей). В blob допускаются только POD-виды; `string`/`array`/`desc_ref` хранятся как индекс в таблице хендлов (§3). Реестр — `desc::schema_registry` внутри `desc_system` (ADR-04: «реестр типов в desc_system»). Он заполняется в `register_desc<T>` (если у `T` есть описание Describe) и в `register_schema(type_schema)`. Тип только со схемой регистрируется в той же `factory_map` (`desc_system.h:46-50`) лямбдой, которая захватывает схему и создаёт `desc::dynamic_desc{schema, blob}`. Снятие регистрации — через `registration_scope` (DISTRIBUTION §2, `registration_context`).

**2.3. Generic-операции** (`desc_schema_ops.h`, #129). Для каждого поля строится `field_ops{to_json(const void*, json::value&), from_json(void*, const json::value&) -> bool, equal(const void*, const void*), assign}`. Для C++ это указатели на шаблонные функции через member pointer, для runtime — по `offset` в blob. На этом общие функции:
- `serialize(schema, obj, out)`: обход полей, запись по `key` через `json_pointer_set` (`desc_json_ops.h:95`) по политике `write`. Порядок ключей — порядок полей. Порядок задаётся так, чтобы совпасть с нынешним выводом: файлы должны остаться побайтно прежними (pretty-печать `desc_system.h:200+`).
- `deserialize(schema, obj, in)`: нет ключа (и нет alias) — поле **не трогается**. На этом держится наслоение «copy родителя + deserialize overrides» (§1). Битое значение даёт предупреждение и оставляет текущее значение (как у света сейчас). Неизвестные ключи игнорируются с логом уровня debug.
- `copy(schema, src, dst)`: если схемы совпадают — поэлементное присваивание. Если нет — через JSON (serialize src → deserialize dst), пересекаются только общие ключи, с предупреждением. В мосте `desc_base::copy_to` перестаёт быть чистым виртуальным: по умолчанию проверяет `get_type()`/схему и при несовпадении копирует через JSON. UB уходит в обоих местах вызова (`desc_adapter_t.cpp:74`, `scn_ecs_assembler.cpp:66`), в том числе у рукописных типов.
- `diff(schema, base_json, edited_json) -> vector<{pointer, value}>`: сравнение **по полям схемы** (`equal` с относительным допуском для float (`desc::float_relative_tolerance`, `|a-b| <= eps*max(|a|,|b|)`), `array`/`custom` — целиком, `structure` — рекурсивно). Результат — список pointer-записей, а не объект. Его же использует `override_edit::write_component` (сейчас там бестиповой `diff`, `edt_override_edit.cpp:197`); без схемы остаётся нынешний путь.
- `validate_override(schema, record)`: pointer записи должен указывать на поле (или внутрь `structure`/`array`), значение должно приводиться к виду поля. Неизвестный pointer — предупреждение «устаревшее переопределение», запись **не удаляется** (данные не теряются молча).

**2.4. Presence и override-записи — правила, которые generic-код обязан соблюдать.**
1. Структура desc — это **разрешённые значения**, presence в ней не хранится. Признак «задано» живёт только в JSON: `overrides` и записи (DATA_FORMATS_V2 §3). Маска полей в C++ не вводится.
2. Generic-`serialize` не выбрасывает поле из-за равенства умолчанию, пока это явно не указано (`write = if_not_default`). Иначе родитель перестанет «называть» значение, и запись `set` с умолчанием станет неотличима от отсутствия записи.
3. Редактор пишет запись только для изменённого pointer'а. Копирование всех полей родителя при первой правке (`edt_inspector_panel.cpp:156-163`) заменяется на `diff` → записи. Сброс (`reset`) удаляет записи.
4. `transform` остаётся псевдокомпонентом со встроенной схемой `transform{position, rotation(degrees), scale=1}` — для инспектора и `validate_override`. `has_transform` и запись пустого `transform` остаются в `prefab_desc::serialize` (`scn_prefab_desc.cpp:249-259`), generic-код их не касается.
5. Сам `prefab_desc` (узлы, записи, `__parent`) на схему **не переводится**: это контейнер-документ, а не набор полей.

**2.5. Generic-инспектор** (#130, после #138). Порядок выбора: кастомный рендерер из публичного реестра #138 → схема → нынешний generic-JSON. По схеме: виджет выбирается по `kind` и подсказкам (`color` → ColorEdit, `range` → Drag с границами, `enumeration` → Combo, `res_tag`/`desc_ref` → поле ассета с `filter`, `degrees`). Значения — эффективный JSON (`component_json` + записи), правка → `diff` → записи. Метка «переопределено/наследуется» и «сброс поля» ставятся по `has_record(owner, target, component, pointer)`. Поля `editor_hidden` не показываются, `read_only` выключены. Тот же код работает для runtime-схем, поэтому wasm-компоненты сразу получают инспектор.

**2.6. Миграции.** Отдельный реестр не строим, #131 «подключить реестр» уже выполнено (#86). Добавляется: (а) при `register_schema` проверяется `schema.version == current_version(name)` (assert); (б) переход типа на схему **не повышает** версию: формат побайтно тот же, проверка — `desc_resave --check`; (в) хелперы шагов `migrations::rename(from, to)`, `remove`, `set_default` — их же объявляют runtime-схемы декларативно (`"migrations": [{"from": 1, "rename": {"/hp": "/health"}}]`); (г) переименование поля в схеме = новый шаг миграции. Тест: каждый ключ, переименованный в `field_attrs`, покрыт шагом.

**2.7. Какие три типа первыми (#131) и почему.**
1. `directional_light_desc`: 4 × `vec4`, компонент, есть кастомный рендерер (`edt_cr_light.cpp`). Минимальный риск, проверяет `color`/`range`, политику `always` и «битое значение → умолчание».
2. `camera_desc`: приватные члены с другим `key`, вложенный `/transform/*`, `read_aliases` (`/texture`), `write = if_valid` для тегов, `custom`-кодек для `fixed_vector`. Исправляет расхождение умолчаний с редактором. Компонент.
3. `material_desc`: два enum (`pass_queue` со строчными именами, `render_mode`), вложенный `/uniforms`, массивы тегов и строк. Убирает дублированную таблицу строк (`scn_material_desc.cpp:17`) и закрывает «enum ↔ string» на реальном типе. Есть `scn_material_desc_tests.cpp`.
Четвёртым переведён skybox (ссылка `desc_ref`, см. «Skybox (#131-d)» в конце). Дальше: anchor/object/bone/animated_node (простые), затем текстуры (6 enum), skin/animation. **На схему не переводятся:** prefab/world/level (документы), geometry (массивы чисел по ~10 МБ, свой кодек). Список непереведённых ведётся в описании #16.

## 3. Компоненты без C++-типа в ECS (направление; реализация — S12 #230, S3 #220)

Что нужно: создать хранилище по схеме, адресовать его по имени (не `type_hash`), чтобы работали views/удаление сущности/снимки, и дать пакетный доступ для wasm (DISTRIBUTION §7.1: «один вызов на систему с пачкой»).

| Вариант | Как | За | Против |
|---|---|---|---|
| **A. Именованное хранилище EnTT одного C++-типа** | `reg.storage<ecs::dyn_blob>(schema.id)`: у EnTT несколько пулов одного типа по `id_type`. `dyn_blob` — байты размера `schema.layout` (SBO до 64 Б, больше — в куче) | Работают views по пулу, `destroy`, сигналы, `component_tracker`. Не нужно трогать EnTT. Мало кода | Пакетная выдача = gather/scatter по сущностям. `id_type` 32-битный — коллизии проверять при регистрации |
| **B. Своё хранилище со stride** | Наследник `entt::basic_sparse_set` с плотным `vector<byte>` и шагом `layout.size` (swap-and-pop параллельно sparse set) | Чанк — один `memcpy` в линейную память wasm. Лучшая локальность | Вставить свой пул в `entt::registry` публичным API нельзя. Нужна проверка спайком на EnTT 3.15 и, возможно, патч |
| C. Meta-тип в рантайме | `entt::meta` без C++-типа | — | Невозможно: meta и storage требуют C++-тип. **Отклонено** |
| D. Хранилище вне registry | Карта `entity → row` у движка | Полный контроль | Нет views/снимков, удаление синхронизировать вручную. **Отклонено** |

**Рекомендация:** A сейчас, за интерфейсом `ecs::dyn_components` (`emplace(e, schema_id)`, `bytes(e, schema_id)`, `for_each_chunk(schema_id, fn(span<const entity>, span<byte> data, stride))`). B — только если спайк S12 покажет, что gather дорог. Интерфейс от варианта не зависит. Wasm не видит память хоста, копирование в линейную память нужно в любом случае. Значит, выигрыш B ограничен.

**Ограничения для будущего host API (зафиксировать до S3):**
- Id компонента — `fnv1a_64(stable_name)` из схемы (усечение до `entt::id_type`, коллизия — ошибка регистрации). C++-компоненты переходят на те же имена в S1/S3 (`ecs_component.h:38`).
- Раскладка blob'а — C-раскладка по `field_schema.offset`, little-endian, только POD. `string`/`res_tag`/`desc_ref` — `u32`-хендл в таблице строк и ресурсов хоста. Сущность — `u32` версия+индекс, не `entt::entity` напрямую.
- Пакетный вызов на систему: хост копирует чанк(и) выбранных схем в линейную память, вызывает экспорт, пишет обратно только схемы, объявленные на запись (как `system_access`).
- Схема несёт `version`. Hot reload модуля с другой раскладкой = миграция blob'ов: JSON-сериализация по старой схеме → шаги → десериализация по новой. Это те же generic-операции.
- desc ↔ компонент для runtime-схемы — тождество: `dynamic_desc` и blob в ECS имеют одну раскладку. Generic-спавнер копирует байты, `patch` при hot reload — тоже.

## 4. План по PR (каждый — CI зелёный; `desc_resave Editor/res --base core/res --check` и `core/res` → 0 изменений)

Место в волнах — STAGE_D_PLAN §2: C (#128→#129) — после PR-5 #109. Заголовки desc переезжают в PR-2 (`HEADER_SPLIT.md:73`), новые файлы сразу создаются в `include/snk/desc/`. #130/#131 — во 2-й волне, после #138.

1. **S0 (до волны 1, мелкий):** добавить `desc_resave --check` по `core/res` и `Editor/res` в CI. Это защита байтовой совместимости на весь эпик.
2. **#128:** `describe`, `mp11` в `ThirdParty.cmake:89`; `desc` линкует `Boost::describe Boost::mp11` PUBLIC (SDK-замыкание ставит заголовки в `include/snk/boost`), проверка `check-sdk-headers` и `package_consumer`. `desc_schema.h`, `desc_hints.hpp`, `field_attrs`, `schema_registry`, `parse_type_schema`. Тест на тестовом типе: C++-схема и JSON-схема одного типа совпадают.
3. **#129:** `desc_schema_ops` (serialize/deserialize/copy/equal/diff/validate_override, enum ↔ string), `dynamic_desc`, мост `desc::schema_desc<T>` (CRTP реализует три виртуальных метода), `copy_to` с проверкой типа для всех 21 типа (UB). Тесты: round-trip без рукописного кода, тип только со схемой (спавн через `factory_map`), alias, `if_valid`, наслоение parent+overrides, неизвестный pointer.
4. **#131-a/b/c:** light → camera → material, по PR на тип. В каждом: удалить рукописные `serialize/deserialize`, проверить `desc_resave --check` и `desc_repo_roundtrip_tests`, добавить тест «схема = прежний вывод» на эталонном JSON.
5. **#130 (после #138):** schema-инспектор в Editor как запасной путь реестра #138; запись через `diff` → записи (исправляет утечку presence). Удаляются `edt_cr_light.cpp`/`edt_cr_camera.cpp` (их поля покрывает схема), остаются skin/animation.
6. Затем — остальные типы по списку §2.7. ECS-часть §3 — в S12/S3, не в #16.

**Не связано:** #212 и #13 (RHI) эпик не затрагивают. С #18 общий только реестр инспекторов (#138).

## 5. Риски и вопросы владельцу

**Риски:**
- (a) Побайтная совместимость: порядок ключей, `int` против `double` (`desc_system.h:282`), политика записи. Защищают S0 и тесты на эталонном JSON.
- (b) Время компиляции от Describe/mp11 в заголовках desc. Описания и атрибуты держать в `.cpp` типа, в заголовке — только `BOOST_DESCRIBE_CLASS`.
- (c) Приватные члены: Describe их видит только при описании внутри класса. Конфликт с `SNK_API`/экспортом проверить на MSVC shared.
- (d) Float-сравнение в `diff` порождает лишние записи. Нужен допуск и тест.
- (e) Конфликт #130 с #138/#192/#195 по `edt_inspector_panel.cpp`. Решение: строго после #138.
- (f) Решение по EnTT-хранилищу (§3) зависит от спайка S1/S12. Интерфейс `dyn_components` от него изолирован.

**Вопросы (≤6):**
1. Отказаться от собственного макроса `DESC_FIELDS`: поля описывать через `BOOST_DESCRIBE_CLASS`, атрибуты — `constexpr`-таблицей `field_attrs<T>`? **Рекомендация: да**, один стандартный механизм, имя DESC_FIELDS остаётся за подсистемой.
2. `type_schema` — данные времени выполнения, одни и для C++, и для wasm/JSON (Describe только генерирует)? **Рекомендация: да**, иначе требование DISTRIBUTION §7 не выполнить.
3. Presence хранить только в JSON (overrides + записи), без маски полей в структурах desc? **Рекомендация: да**, это совпадает с DATA_FORMATS_V2 §3 и не меняет формат.
4. Первые три типа — light, camera, material (skybox — четвёртым); prefab/world/level/geometry не переводить? **Рекомендация: да.**
5. Компоненты без C++-типа — вариант A (именованный пул `dyn_blob`) за интерфейсом `dyn_components`, B — по итогам спайка S12? **Рекомендация: да.**
6. Добавить `desc_resave --check` в CI отдельным PR до #128? **Рекомендация: да**, это дёшево и закрывает главный риск эпика.

## Решения владельца (2026-10-18, «по рекомендациям»)

1. Свой макрос `DESC_FIELDS` не вводим: поля — `BOOST_DESCRIBE_CLASS`, атрибуты — `constexpr`-таблица `field_attrs<T>`.
2. `type_schema` — данные времени выполнения, общие для C++ и wasm/JSON.
3. Presence — только в JSON (overrides + записи), без маски полей.
4. Первые типы: light → camera → material, skybox четвёртым; prefab/world/level/geometry не переводятся.
5. Компоненты без C++-типа — вариант A (`dyn_blob` в именованном пуле) за `dyn_components`; B — по итогам S12 #230.
6. `desc_resave --check` в CI — отдельным PR (S0) до #128.

## Известные ограничения (#128)

- Схемы вложенных C++-структур не регистрируются автоматически: схема runtime-типа с полем `structure` требует вложенную схему в реестре.
- Атрибуты `field_attrs<T>` сопоставляются по имени члена, а не по указателю (приватные члены); опечатка в имени ловится `static_assert` при генерации схемы.
- Декларативные миграции runtime-схем (`"migrations"` в JSON) пока не поддерживаются: `parse_type_schema` отвечает ошибкой; версия сохранённой схемы следует за `register_migration`.
- Равенство схем (`operator==`) не учитывает `layout` и `offset`: это детали хранения.
- Фабрика схемы без C++-типа создаётся в `register_schema` (#129): вложенные схемы (`structure`) и enum-схемы должны быть зарегистрированы раньше; иначе тип остаётся только в реестре схем, без фабрики, и в лог пишется ошибка.

## Реализация #129

- `desc_schema_ops.h`: `schema_binding` (схема + `field_ops` по полям: `to_json`, `from_json -> bool`, `equal`, `assign`) и функции `serialize`, `deserialize`, `copy`, `equal`, `diff`, `validate_override`, `json_equal`, `enum_from_json`/`enum_to_json`. Привязка к C++-типу (`bind_described<T>`, указатели на члены) — в `desc_describe_ops.h` (тяжёлый, как `desc_describe.h`); привязка к runtime-схеме (`bind_runtime`, смещения в blob) — в `desc_schema_ops.cpp`.
- `dynamic_desc` (`desc_dynamic.h`): `desc_base` поверх blob'а; поля `string`/`res_tag`/`desc_ref`/`array`/`custom` лежат в таблице значений blob'а (в самом blob'е — индекс, 0 = не задано).
- `schema_desc<T, Kind>` (`desc_schema_desc.h`): CRTP-мост, реализует `serialize`/`deserialize`/`copy_to` через схему. На него переведён `directional_light_desc` (#131-a, ниже); остальные типы — рукописные.
- `desc_base::copy_to` больше не чисто виртуальный. У всех 21 рукописного типа тело — `copy_to_same(*this, other)`: тот же тип копируется поэлементно, любой другой — через `serialize`/`deserialize` с предупреждением (нужен `desc_system`, который `create_instance` запоминает в экземпляре; без него копирование пропускается).
- Кастомный кодек поля с видом `custom`: специализация `desc::json_codec<T>`.

## Реализация #130

- `Editor/code/editor_system/panels/edt_schema_inspector.{h,cpp}` (`edt::schema_ui`): виджет по `describe_widget(field, enum_known, nested_known)` — только из вида и подсказок, поэтому проверяется без ImGui. `bool` → Checkbox; `i32/u32/i64/u64` → DragScalar (границы и шаг из `range`); `f32/f64` → Drag (формат из шага, `degrees` дописывает ` deg`); `vec2/3/4`, `quat` → Drag по компонентам; `color` (вид или флаг на `vec3/vec4`) → ColorEdit3/4; `enumeration` → Combo по `enum_schema`; `string` → InputText; `res_tag`/`desc_ref` → InputText, принимающий перетаскивание ассета (`ASSET_PATH`, с проверкой `filter`); `structure` → дерево по вложенной схеме; `array` → дерево с элементами по `element` (добавить/удалить, до 50 строк); `node_ref`/`entity` → только показ; `custom` и значение не той формы → generic-JSON-редактор. `editor_hidden` не рисуется, `read_only` выключен, `tooltip` — подсказка. Поле `optional` без значения показывается как «not set» с кнопкой «Set».
- Значения — схемные умолчания, поверх них данные родителя и `overrides` (`compose`; `read_aliases` читаются как ключ поля). Правка — это `diff(before, after)`: `apply_edit` пишет в `overrides` только изменённые pointer'ы, а поле, вернувшееся к унаследованному значению, теряет свой pointer. Pointer'ы, которых правка не касалась, не трогаются (явное «задано» сохраняется).
- Утечка presence (`edt_inspector_panel.cpp:156-163`) закрыта и для кастомных рендереров: первая правка берёт у рендерера разницу с данными родителя, а не копию всех его полей.
- `camera_desc` и `directional_light_desc` получили описание Boost.Describe и `field_attrs<T>`; их схемы регистрируются в `register_scene_types`, умолчания берутся из членов (fov 90, near 0.0001, far 1000). Сериализация камеры осталась рукописной (перевод — #131-b); свет переведён в #131-a.
- `edt_cr_camera.cpp` удалён: схема покрывает его поля (fov, near, far) и добавляет позицию и повороты. `edt_cr_light.cpp` тоже удалён: схемный виджет `vec4` показывает все четыре компоненты (старый рендерер правил только xyz/rgb и не трогал `w`). Инспектор камеры в снимке checklist (выбрана Game Camera) рисуется иначе, чем рендерер: diff по `checklist.png` 1.823% вместо 0.000% (лимит 2%); референс не менялся.
- Метка «переопределено» и сброс отдельного поля не сделаны: `has_record` пока на уровне компонента.

## Реализация #131-a (свет)

- `directional_light_desc` наследует `schema_desc<directional_light_desc, schema_kind::component>`; рукописные `serialize`/`deserialize`/`copy_to` удалены. `field_attrs<directional_light_desc>` объявлен перед классом, чтобы сгенерированный сериализатор видел атрибуты.
- Вывод прежний побайтно: ключи в порядке `direction`, `diffuse`, `ambient`, `specular`; запись всегда (`write = always`, умолчание не отбрасывается); числа — `float` → `double`, как раньше у `json::value_from(glm::vec4)`. Рукописное чтение уже совпадало с общим: нет ключа — поле не трогается, массив не из четырёх чисел (или с нечисловым элементом) — предупреждение и текущее значение. Алиасов и устаревших ключей у света не было.
- Тесты: `DirectionalLight_SerializedBytesMatchReference` (эталон снят с рукописного кода до перевода и подтверждён тем же CI), `DirectionalLight_AcceptsLegacyForms`, `desc_repo_roundtrip_tests`, `desc_resave --check` в CI.
- Осталось по #131: камера (#131-b), материал (#131-c), четвёртым skybox. prefab/world/level/geometry не переводятся.

## Реализация #131-b (камера)

- `camera_desc` наследует `schema_desc<camera_desc, schema_kind::component>`; рукописные `serialize`/`deserialize` заменены схемой, `copy_to` остался прежним (`copy_to_same`: поэлементная копия через `operator=` переносит и имя типа экземпляра, чего копия по схеме не делает; тест `DescCopyTo_CopiesDataButNotIdentity`), `field_attrs<camera_desc>`, `enum_names` и кодек объявлены перед классом. Схема та же, что давал #130 (ключи `/fov`, `/transform/rotation`, `/transform/position`, `write_if_valid` у тегов, `write_if_not_default` у полей W4); инспектор не меняется.
- Вывод прежний побайтно: порядок ключей — порядок членов (`fov`, `near_distance`, `far_distance`, `transform{rotation, position}`, затем цели рендера и поля W4); `transform` пишется всегда; `color_targets` — только непустой массив, три тега — только непустые строки; `layer`, `clear`, `projection`, `ortho_size` — только если не равны умолчанию (сравнение точное: `json_equal` без допуска).
- Кодек `json_codec<ds::fixed_vector<res::tag, 4>>` (поле `color_targets`, вид `custom`): пишет массив строк; читает массив или одну строку (это устаревший ключ `"texture"`, он же `read_alias`), тег без протокола получает `memory://`; больше четырёх тегов не помещаются (предупреждение, значение не меняется; раньше `fixed_vector::push_back` бросал исключение).
- Четыре места, где общий читатель отличался от прежнего, закрыты в `camera_desc::deserialize` до вызова общего чтения (по копии объекта), без изменения `core/core/desc`: теги `depth_target`/`tp_accum_target`/`tp_reveal_target` без протокола получают `memory://` (у `res_tag` нет такой нормализации, а `res::tag(строка)` режет мусор из строки без протокола); `clear` и `projection`, которые не строка, игнорируются (общий читатель принял бы число, названное индексом, например `"projection": 1`); `layer` читается прежним кодом (`if_int64`, зажим до ±16, `uint64` даёт 16, не целое, в том числе `3.0`, игнорируется с предупреждением; общий читатель принял бы `3.0` и оставил бы текущее значение при `100`); `enum_names` камеры без `fallback`, чтобы неизвестное слово оставляло значение, а не ставило умолчание.
- Единственное намеренное расхождение — только на битом входе, который сохранение не порождает: `rotation`/`position` не из трёх чисел раньше обнуляли поле, теперь предупреждение и текущее значение (как у света в #131-a); не число в `fov`/`near_distance`/`far_distance`/`ortho_size`, не объект в `transform` и не строка в теге раньше бросали исключение, теперь предупреждение или пропуск.
- Тесты: `CameraDesc_SerializedBytesMatchReference` (эталон снят с рукописного кода до перевода и подтверждён тем же CI), `CameraDesc_OnDiskTextMatchesReference` (через `write_pretty_json`), `CameraDesc_AcceptsLegacyAndMalformedForms`, прежние `CameraDesc_*`, `desc_repo_roundtrip_tests`, `desc_resave --check` в CI.
- Осталось по #131: четвёртым skybox (материал и skybox переведены, см. «Материал (#131-c)» и «Skybox (#131-d)» в конце).

## Расширение схемы для material/skybox (2026-10-20, решение владельца)

`material_desc` и `skybox_desc` нельзя перевести (#131-c/d) без трёх возможностей, которых в схеме не было. Решение владельца: добавить их в `core/core/desc`, не трогая вывод света и камеры (`desc_resave --check` — 0 изменений). Сами material и skybox в этом PR не переводятся.

- **Явный `null` при записи.** Материал пишет `"shader_geometry": null` для пустого тега и `"queue": null` для `pass_queue::NONE` (`core/res/base_material.desc`, `core/res/skybox/sky_material.desc`); общий `serialize` пропускал `null`, а `res_tag` писал `""` и не читал `null`. Политика `write_policy::null_if_empty` (`field_attr::write_null_if_empty()`, имя в JSON-схеме `"null_if_empty"`): значение пишется как есть, но пустое (пустой тег, строка, массив; у enum — элемент `enum_names<E>::null_value` / `"null_item"` в JSON-описании enum) пишется явным `null`. Чтение: `null` в поле с этой политикой — пустой тег или строка (`field_ops::accepts_null`); `null` в enum читается как `enum_schema::null_value`, без него — не подходит, как раньше; `null` в массиве очищает его. `enum_from_json` сам `null` не понимает: это решение поля с политикой, поэтому enum с `null_value` в поле без политики `null` по-прежнему отвергает. `json_fits(..., accepts_null)` и `validate_override` учитывают политику поля, так что override-запись `{pointer, null}` у такого поля допустима (у остальных — `bad_value`). В поле без политики `null` по-прежнему не подходит (предупреждение, значение не меняется). Работает и для runtime-схем.
- **Ключ только для чтения.** `uniforms.shininess` читается, но не пишется. Политика `write_policy::never` (`write_never()`, `"never"`): `serialize` поле пропускает, а `deserialize`, `copy` (того же типа) и `equal` видят его как обычный член. Так как в сериализованном JSON ключа нет, он не попадает в `diff` и не может быть override-записью. Не путать с флагом UI `read_only` (поле показано, но выключено): `never` — про файл, `read_only` — про инспектор. Копия между разными типами через JSON такой ключ тоже не переносит, копия того же типа переносит. Инспектор не показывает поле с `never` (`describe_widget` даёт `widget::hidden`): правка ключа, который не сохраняется, потерялась бы при первом же сохранении.
- **`desc_ref` с контекстом чтения.** Скайбокс читает материал через `desc_system.get_field_desc<material_desc>(*this, resource.at("material"))`: строка-тег или inline-объект с `__type` (`make_mem_tag_deterministic(owner, type, data)` → `store` → `require`). Общий путь `desc_system` отбрасывал. Теперь `schema_desc::deserialize` передаёт в `desc::deserialize(binding, obj, in, read_context{ &desc_system, this })` контекст (`desc_system*` и владелец-desc), `field_ops::from_json` принимает `const read_context&`, а кодек вида `custom` может объявить третий параметр `from_json(T&, const json::value&, const read_context&)`. Для члена `res::res_handle<T>` с `f("...").kind(field_kind::desc_ref).ref("material_desc").write_if_valid()` работает `desc_ref_traits<res::res_handle<T>>` (`desc_describe_ops.h`): строка (тег) или объект с `__type` разрешаются через `get_field_desc<T>`, пустая строка сбрасывает хендл, остальное и чтение без контекста не подходят (предупреждение, хендл не меняется), пишется тег хендла (пустая строка, пока ссылка не задана, — вместе с `write_if_valid` ключ не пишется). Умолчание у `desc_ref`-поля `null`. Хендл, чей тег — `memory://overrides/…` (копия inline-объекта, живёт только в памяти процесса), пишется обратно inline-объектом (`serialize` загруженного desc + `__type`), иначе сохранённый тег не разрешился бы в новом процессе; `res://`-теги пишутся тегами. Inline-объект у владельца без тега не принимается (имя копии строится из тега владельца). С `write_null_if_empty()` поле `null` сбрасывает хендл. Тип, на который указывает `desc_ref`, должен объявлять `using base_type = desc::desc_base;` (так `resource_system` находит адаптер desc; у material/skybox/camera он есть), а `desc_adapter_t` должен быть зарегистрирован в `resource_system` (в движке и в `scene_env` он зарегистрирован). Пока `desc_ref` — только сам `res::res_handle<T>`: элементы `std::vector<res::res_handle<T>>` остаются полями вида `custom`. Инспектор показывает `desc_ref` как поле ассета (как `res_tag`, `widget::asset`).
- **Инспектор и null-enum.** Поле enum с `null_if_empty` и `null_value`: `null` в значении показывается как этот элемент, а выбор его в списке пишет `null` (а не имя).
- **Совместимость.** Существующие типы (свет, камера) политик не используют, их вывод побайтно прежний. Цена подключения `desc_ref`: `desc_describe_ops.h` теперь включает `desc_system.h`.
- Тесты: `tests/desc_schema_ops_tests.cpp` — `WriteNullIfEmptyWritesAnExplicitNullAndReadsItBack`, `NullIsOnlyAcceptedWhereTheSchemaDeclaresIt`, `NeverWrittenKeyIsReadCopiedAndComparedButNotWritten`, `RuntimeSchemasHonorNullIfEmptyAndNever`, `DescRefReadsATagAndWritesIt`, `DescRefReadsAnInlineObjectUnderADeterministicMemoryTag`, `DescRefKeepsItsHandleWhenTheValueDoesNotFit`, `DescRefIsTheKindOfTheFieldInTheSchema`.

## Материал (#131-c, 2026-10-20)

`material_desc` описан схемой (`schema_desc<material_desc, schema_kind::desc>`, `scn_material_desc.h`); рукописные `serialize` и таблица строк `render_mode` удалены, инспектор строится из схемы. `deserialize` остался только ради одной формы: `queue` не строка и не `null` читается как `NONE` (раньше любое неподходящее значение давало «нет очереди», а общий читатель принял бы число, называющее элемент enum). Файлы `core/res/*.desc` и порядок ключей — побайтно прежние (`MaterialDesc_ShippedFilesResaveToTheSameBytes`, `desc_resave --check`).

| Ключ в файле | Член | Политика записи | Заметка |
|---|---|---|---|
| `defines` | `defines` | `always` | пустой массив пишется `[]` |
| `samplers` | `samplers_textures` | `always`, вид `custom` | кодек: пустой тег ↔ `null` (пустой слот `[null, "…"]` пишет импортёр); массив схемы не принял бы `null` в элементе |
| `shader_vertex` / `shader_fragment` / `shader_geometry` | `*_shader` | `null_if_empty` | пустой тег пишется `null`, читается `null` и `""` |
| `queue` | `queue` | `null_if_empty` | слова `opaque/transparent/mix`; `NONE` = `null_value`, пишется `null`; неизвестное слово — `fallback = NONE` |
| `uniforms/albedo`, `uniforms/emissive` | `albedo`, `emissive` | `always` | `color` |
| `uniforms/shininess` | `shininess` | `if_not_default` | **#321, решение владельца (б)**: пишется, когда не равен 32 (диапазон 0–1000) |
| `render_mode` | `render_mode` | `always` | имена элементов enum; неизвестное слово — `fallback = TRIANGLE` |

- `ambient` и `specular` в файле не было никогда и нет сейчас: в схему они не входят, но `copy_to` (член-в-член) их переносит.
- Политика `never` материалу не понадобилась: shininess не «только читается», а пишется по `if_not_default`. Файл со `shininess: 32` при пересохранении теряет ключ (раньше ключ не писали вовсе).
- Намеренное расхождение — только на битом входе, которого сохранение не порождает: вектор не из четырёх чисел или значение другого типа в теге/числе теперь дают предупреждение и оставляют текущее значение (раньше нулевой вектор, пустой тег или исключение).
- Поле `samplers` в инспекторе — JSON-редактор, а не список ассетов (вид `custom`): цена байтовой стабильности формы с `null` в слотах. Вернуть список можно, научив схему `null` в элементе массива.
- `desc_ref_traits::to_json` (`desc_describe_ops.h`): только главный поток (inline-копия читается `get_sync`); хендл с ошибкой пишется своим тегом без `get_sync` и без лога на каждое сохранение; `static_assert` на `T::base_type`.
- Байты файлов меняются только у материалов с `shininess` не по умолчанию; в репозитории таких `.desc` нет (импортёр создаёт их при импорте модели), см. описание PR.

## Skybox (#131-d, 2026-10-20)

`skybox_desc` описан схемой (`schema_desc<skybox_desc, schema_kind::component>`, `scn_skybox_desc.h`); рукописные `serialize`/`deserialize` удалены, `scn_skybox_desc.cpp` держит только `assemble_skybox`. Кастомный рендерер `edt_cr_skybox.cpp` (текстовое поле «Material Tag») удалён: инспектор строится из схемы, материал в нём — поле ассета. Член `sky_material_desc` переименован в `material`, как ключ в файле. Файлы `core/res/skybox/skybox.desc`, `Editor/res/templates/prefab_preview.desc` и компоненты в уровнях побайтно прежние (`SkyboxDesc_ShippedFileResavesToTheSameBytes`, `desc_resave --check`).

| Ключ в файле | Член | Политика записи | Заметка |
|---|---|---|---|
| `material` | `material` (`res_handle<material_desc>`) | `if_valid`, вид `desc_ref`, `ref("material_desc")` | тег или inline-объект с `__type`; тег пишется тегом, `memory://overrides/…` — inline-объектом; пока материала нет, ключа нет |

- Читаются все прежние формы: строка-тег и inline-объект с `__type` (через `read_context`, который даёт `schema_desc::deserialize`); отсутствие ключа оставляет материал пустым. Дополнительная предобработка в `deserialize` не нужна, поэтому он не переопределён.
- Намеренное расхождение — только на битом входе, которого сохранение не порождает: число, `null`, массив, `true` или объект без `__type` в `material` раньше срабатывали как `ASSERT_MSG`, теперь это предупреждение, и скайбокс оставляет прежний материал. Пустая строка сбрасывает материал.
- Намеренное расхождение для inline-материала: раньше он записывался обратно тегом `memory://overrides/…`, который в следующем процессе не разрешался, теперь записывается inline-объектом (правило `desc_ref`, см. выше).
- Известное ограничение (правило `desc_ref`: inline-объект принимается только у владельца с тегом): временный desc ассемблера (`scn_ecs_assembler.cpp`, `create_instance(type, res::tag{})`) не имеет тега. Если у родителя скайбокса материал inline (его `serialize` теперь пишет объект) и у компонента есть overrides, временный desc материал не прочитает и спавн получит скайбокс без материала. Для `res://`-тегов, то есть для всех файлов репозитория, это не затрагивается.
- Тесты: `tests/scn_skybox_desc_tests.cpp` (побайтный пересейв отгруженного файла в компактном и в файловом виде, тег, inline, inline через сохранение в новую систему, битые формы, `copy_to`, объявление схемы), `SkyboxInspectorFieldComesFromTheSchema` в `edt_schema_inspector_tests.cpp`, `desc_repo_roundtrip_tests`.

### Итог #131

Все четыре типа EPIC-55 (свет, камера, материал, skybox) описаны схемой; `desc_resave --check` не меняет ни одного отгруженного файла. Не переведено и не планируется: prefab/world/level (документы) и geometry (свой кодек). Остаётся за пределами #131: anchor/object/bone/animated_node, текстуры (`texture`, `texture_2d`, `texture_cubemap`), skin/animation и остальные типы сцены, список ведётся в описании #16. Поле `samplers` материала — JSON-редактор (вид `custom`), пока схема не принимает `null` в элементе массива.
