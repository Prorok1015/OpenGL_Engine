# Module API 4.0: что сломано и почему

Состояние на 2026-11-23. **Окно 4.0 открыто до конца этапа 3:** версия остаётся 4.0 (`SNK_MODULE_API_VERSION` = `0x0004'0000`), ломающие изменения идут отдельными PR, а ключ раскладки модуля (`gs::module_layout_key()`) меняется в каждом таком PR. Модуль 3.x и старше движок отклоняет при загрузке: проект надо пересобрать.

Это обзор, а не журнал. Журнал: [`HISTORY.md`](HISTORY.md) (до #1056) и [`changes/`](changes/README.md) (после). Дизайн мажора: [EPIC-57 §7.1](../epics/infrastructure/EPIC-57.md) (#758) и [MODULE_LAYER.md](../roadmap/design/MODULE_LAYER.md).

## Зачем мажор
Идентичность ассета перестала быть `res::tag`: ссылка в данных читается строкой или объектом `{guid, path}` (EPIC-57), а кэш `resource_system` и разбор ссылки перенесены за экспорты движка. Модуль, собранный с inline-доступом к старой раскладке, читал бы чужую память, поэтому один раз поднят мажор и закрыт список мест, где раскладка принадлежит движку.

## Что сломано в самой 4.0 (#758)
- Поле-ссылка читается через экспорты `res::reference_from_json` и `resource_system::resolve_reference`; кэш `resource_system` (`key_for`, `warmup`, `pin_resource`, `push_resource_to_*cache`, `find_cached`, `resolve_alias`) спрятан за экспортами, его раскладка — дело движка.
- `res_handle` хранит кэш проверки типа (#632); `ecs::detail::channel_slot` получает `pending()`, `context_set::pending_mail` считает письма в полёте (#668); `scene_layer` держит `desc_system` указателем.

## Окно 4.0: что ещё ломалось
Каждая строка — отдельный PR; `layout_revision` назван там, где он менялся.

| Изменение | Что для модуля | Запись |
|---|---|---|
| `world_context`: аксессоры — экспорты, поля переставлены (#802) | модуль до окна отклоняется (`layout_revision` 1) | [HISTORY](HISTORY.md) |
| `desc_system`, `schema_registry`, `dynamic_desc` — члены экспортами; `migrate_object/value/typed` 3.16 убраны (#786) | пересборка (`layout_revision` 2) | [HISTORY](HISTORY.md) |
| Мета-функция `loader_get` компонента возвращает `ecs::sandbox_emplace<T>`; `meta_archive`, `snapshot_get` убраны (#784, #738) | пересборка (`layout_revision` 3) | [HISTORY](HISTORY.md) |
| Словари и `std::optional<T>` в схемах (#790) | `layout_revision` 4 | [HISTORY](HISTORY.md) |
| `system_registry` целиком экспорты; `emplace_schema_component` берёт `const dyn_blob&` (#785, #826) | `layout_revision` 5 | [HISTORY](HISTORY.md) |
| Перегрузки 3.15 `inp::action_map::add` убраны (#781); `loaded_module_apis` и др. убраны (#782); `inp::action_binding` — структура `{kind, code, threshold}` в ключе (#783) | пересборка (`layout_revision` 6, 7) | [HISTORY](HISTORY.md) |
| `cmd::command_desc` получает `describe_before_allowed_invoke`, входит в ключ (#759) | `layout_revision` 10 | [HISTORY](HISTORY.md) |
| Ключ перечисляет компоненты движка, ctx-типы, события, каналы и структуры по значению; видит версии EnTT/Boost/glm (#922) | `layout_revision` 11–13; модуль со статическим CRT или без ключа отклоняется | [HISTORY](HISTORY.md) |
| `scn::prefab_instance` получает `content_of` (#993) | `layout_revision` 14 | [HISTORY](HISTORY.md) |
| `ds::task_handle_base`, `ds::task_handle<void>` в ключе: `gs::game_module::run` возвращает хендл по значению (#1060) | `layout_revision` 15 | [1060](changes/1060.md) |
| `platform::process_confinement`: `container_name` заменён общим `container` (#1099) | ключ раскладки остаётся; модуль, который запускает дочерний процесс с confinement, пересобирается | [1099](changes/1099.md) |
| Адаптер модели вынесен из `snk_engine` в `snk::model_import_assimp` (#1108) | модуль, звавший `scn::model_importer_adapter`, переходит на `mdl::model_adapter` | [1108](changes/1108-model-import-module.md) |
| Старые заголовки-перенаправления `res_tag.h`, `res_asset_id.h`, `res_reference.h` и др. убраны; код берёт `<snk/model_codec/...>` (#1161) | правка `#include` | [1161](changes/1161-drop-forwarders.md), [перенос](changes/1161-model-codec-headers.md) |
| `wnd::gl_surface` → `rnd::driver::gl_surface` (#1161) | те же виртуальные функции в том же порядке; `layout_revision` 17 поднят в #1217 | [1161](changes/1161-gl-surface.md) |
| `desc::field_kind::binary_ref`; `rnd::geometry_desc` держит двоичное состояние членом (#1068) | `layout_revision` 16, ключ получает `rnd::geometry_desc`, `res::binary_range`, `rnd::geometry_view`, `rnd::geometry_file_options`; модули пересобираются | [1068](changes/1068.md), [PR5](changes/1068-pr5.md) |
| `register_schema` отказывает ключу поля не-JSON-pointer (#1228) | схема с ключом без `/` не регистрируется | [1228](changes/1228.md) |
| `scn::override_record` получает `type_version` в конце (#1255) | `layout_revision` 24 (поле может лечь в паддинг, ключ сам не меняется); модули пересобираются | [1255](changes/1255.md) |

Минорные добавления (новые экспорты без смены типов: ключ раскладки остаётся) в таблицу не входят, они только в [`changes/`](changes/README.md).

## Когда окно закроется
Записи `changes/` переносятся в `HISTORY.md` под заголовком версии, их файлы удаляются (один PR, см. [`changes/README.md`](changes/README.md)); этот файл тогда сводится к итогу 4.x.
