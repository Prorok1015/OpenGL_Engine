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
| `field_schema` | `name` (метка в UI), `key` — JSON-pointer относительно объекта (`"/fov"`, `"/transform/rotation"`), `kind`, `ref_type` (имя схемы для enum/struct/ссылки), `default` (`json::value`), `range{min,max,step}`, `write ∈ {always, if_valid, if_not_default}`, `read_aliases[]` (`"/texture"`), флаги `optional`, `editor_hidden`, `read_only`, `color`, `degrees`, `tooltip`, `offset` (в blob) |
| `enum_schema` | `name`, `items[{string, int64}]`, `fallback` (значение для неизвестной строки, как `TRIANGLE` в `scn_material_desc.cpp:33`) |
| `field_kind` | `bool, i32, u32, i64, u64, f32, f64, vec2/3/4, quat, color, string, enumeration, res_tag, desc_ref` (тег или inline-desc, как `get_field_desc`), `node_ref` (node_id, hex-строка), `entity` (только runtime), `structure`, `array`, `custom` (собственный кодек типа, например `ds::fixed_vector<res::tag,4>`) |

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
Дальше: skybox (ссылка `desc_ref`), anchor/object/bone/animated_node (простые), затем текстуры (6 enum), skin/animation. **На схему не переводятся:** prefab/world/level (документы), geometry (массивы чисел по ~10 МБ, свой кодек). Список непереведённых ведётся в описании #16.

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
- Осталось по #131: материал (#131-c), четвёртым skybox.
