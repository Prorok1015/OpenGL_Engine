# Дизайн: EPIC-29 — сервисы и готовность к DLL

**Issue:** #6 (stories #58–#62; закрывает и #170) · **Статус:** утверждён владельцем 2026-10-17 (все рекомендации §9 приняты) · **Дата:** 2026-10-17

Реальные пути: `core/core/common/include/snk/common/ds/ds_store.hpp` (+ `ds_type_id.hpp`), `core/core/application/include/snk/application/app_module_interface.h`, `app_module_loader.hpp`.

## 1. Текущее состояние
**#58 ds_store.** Ключ — `ds::type_id::value<T>()` (`ds_type_id.hpp:13-16`), function-static счётчик (`:30-33`): в каждом бинарнике своя нумерация → DLL получит чужой слот. Хранилище `unordered_map<type_id, std::any>` (`ds_store.hpp:131`), порядок разрушения не определён; policy-шаблон (`:13-50`) используется только в `tests/ds_store_tests.cpp:16`; `construct` молча перезаписывает (`:110`); `destruct` отсутствующего — только ASSERT (`:126`); `require` уже бросает в release (`:99-106`). `std::any` ломается через границу .so при hidden visibility (libstdc++ сравнивает указатель на менеджер). `rnd::frame_context::data` — тоже `app_data_storage`, пересоздаётся каждый кадр (`rnd_frame_context.h:12`).

**#59 сборка.** Все таргеты STATIC, `BUILD_SHARED_LIBS OFF FORCE` (`cmake/ThirdParty.cmake:12`); нет PIC, visibility, export-макросов; есть INTERFACE `core_api`/`engine_api`.

**#60 game_system.** Класс пуст (`gs_game_system.cpp:34-41`); глобалы `p_game_system` (`:12`), `skin_manager` (`:21`, не используется), `gMaxTexture2DSize` (`:13`, пишется в `rnd_gl_render_context.cpp:6,23`, не читается). `gs_game_init.cpp:24-167` регистрирует 17 desc и 14 спавнеров, `game_term` (`:170-203`) снимает только 10 и 13 (не снимаются `prefab`, `world`, `level`, `anchor`, `camera`, `directional_light`, `skybox` и спавнер `world_desc`). Дубль регистраций в `tests/test_scene_env.hpp:154-214`. Редактор не снимает `model_importer_adapter` (`editor_module.cpp:31`).

**#61 get_system().** `res::get_system()` — 17 вызовов, все в глубоком коде: `scn_model_importer_adapter.cpp` (11), `scn_prefab_desc.cpp:207,328`, `rnd_scene_shader_desc.cpp:24,89`, `rnd_texture_manager.cpp:16`, `rnd_geom_manager.cpp:26`. `rnd::get_system()` — 3: `configure_pass` (`rnd_render_system.h:77`, 5 пассов), `scn_skin_weights_desc.cpp:52` (#170), `gs_game_system.cpp:40`. `gui::get_system()` — 0 (мёртвый).

**#62 модули.** `module_interface` (`app_module_interface.h:15-30`): 3 фазы, `MODULE_PRIORITY`, `depends_on`; `module_loader`: топосорт Kahn, откат init, обратный shutdown; состояния модуля нет, загрузить/выгрузить один модуль нельзя. Инверсия: `engine_module.cpp:23` вызывает `loop.init()` у сервиса редактора.

## 2. #58 — ключи и порядок разрушения
- `ds::type_key<T>()`: constexpr FNV-1a 64 от `std::source_location::current().function_name()`; если у типа есть `static constexpr std::string_view service_id` — ключ от него. Стабилен в пределах тулчейна (игровая DLL обязана собираться тем же компилятором — см. §6).
- Хранилище без policy и `std::any`: `vector<entry{key, name, void* ptr, shared_ptr<void> owner}>` + индекс `key→pos`; `require_shared` отдаёт aliasing `shared_ptr<T>` — 117 мест использования не меняются.
- Проверки во всех сборках: повторный `construct` — ошибка; коллизия хеша (другое имя) — ошибка; `destruct` отсутствующего — исключение; `size()`.
- Разрушение — строго обратно порядку регистрации (LIFO).
- Тесты: LIFO, повторная регистрация, стабильность `type_key`.

## 3. #59 — PIC, visibility, SNK_API
- Сейчас (бинарник не меняется): `CMAKE_POSITION_INDEPENDENT_CODE ON` глобально до `include(ThirdParty)`; hidden visibility только нашим таргетам через `snk_target_defaults(tgt)` (`cmake/SnkTarget.cmake`); ручной `core/core/common/include/snk/common/snk_api.h` с одним макросом `SNK_API` (пустой в статике).
- Shared-вариант: опция `SNK_ENGINE_SHARED` (OFF), один `snk_engine SHARED` из компонентов core+engine через `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` (минимум CMake → 3.24); CI-задача «Linux GCC Shared»; Windows MSVC shared собирается и тестируется в CI («Windows MSVC Shared», #198).
- Что ломается в shared: typeinfo для исключений/`dynamic_cast` (экспорт классов); синглтоны (`cfg::config_manager::instance`, реестр spdlog, профайлер, `thread_local` frame allocator) — жить только в `snk_engine`; `entt::meta` контекст в статическом `locator` каждого бинарника — хост передаёт `entt::locator<entt::meta_ctx>::handle()` в модуль.

## 4. #60 — registration_scope вместо game_system
```cpp
namespace scn {
class registration_scope {               // move-only
public:
  registration_scope(desc::desc_system&, ecs_assembler&);
  ~registration_scope();                 // снимает всё в обратном порядке
  template<class T> void desc(std::string_view type, bool component = false);
  void spawner(std::string_view type, desc_spawner_fn, desc_patcher_fn = {}, spawner_category = {});
  std::size_t size() const;
};
registration_scope register_scene_types(desc::desc_system&, ecs_assembler&, rnd::skinning_manager&);
}
```
- Спавнер `skin_weights` захватывает `skinning_manager&` → закрывает #170.
- `engine_module` держит `std::optional<registration_scope>`, сбрасывает до `scene_term`. Render-регистрации — такой же scope; `test_scene_env.hpp` вызывает `register_scene_types`.
- Удаляются `gs_game_system.*`, `gs_game_init.*`, глобалы `p_game_system`, `skin_manager`, `gMaxTexture2DSize`.

## 5. #61 — DI
- Composition root: `*_service_init.cpp`, `editor_module.cpp`. Через конструктор: `ecs_assembler(desc, res)` (аксессор `resources()` для `scn_prefab_desc.cpp:207,328`); `model_importer_adapter(desc, res)` (+ снятие в shutdown редактора); `texture_manager`/`geom_manager`/`ShaderManager` через `render_system(driver, desc, res)`; `shader_config::load(res&)`. `configure_pass` → `ctx.shaders->use(desc)`.
- Тесты передают `ram_res_env` в конструктор, `global_res_guard` удаляется.
- `get_system()` — `[[deprecated]]` в первом коммите, удаление вместе с `p_res_system`, `p_gui_system`, `p_render_system` в последнем.

## 6. #62 — жизненный цикл и C-точка входа
- Состояния: `added → registered → initialized → shut_down → unloaded`. `load(module)` — зависимости уже `initialized`; `unload(name)` — отказ, если от модуля зависит инициализированный; проверка `storage.size()` до/после (асимметрия — ошибка); handle библиотеки освобождается последним.
- Убрать `MODULE_PRIORITY` (ничья — по вставке). `loop.init()` → `application::run`.
```cpp
#define SNK_MODULE_API_VERSION 0x0001'0000u           // major<<16 | minor
struct snk_module_build_info { uint32_t api; uint32_t compiler; uint32_t flags; };
extern "C" SNK_MODULE_EXPORT uint32_t snk_module_api_version();
extern "C" SNK_MODULE_EXPORT const snk_module_build_info* snk_module_build_info();
extern "C" SNK_MODULE_EXPORT app::modules::module_interface* snk_create_module();
extern "C" SNK_MODULE_EXPORT void snk_destroy_module(app::modules::module_interface*);
#define SNK_DECLARE_MODULE(Type) /* все четыре функции */
```
- ABI: C-имена, за ними C++-тип и тот же тулчейн/CRT; несовпадение major или `build_info` — отказ. Уничтожает тот бинарник, что создал.
- Сейчас: `module_entry` из указателей на функции, тесты load/unload в unit_tests. dlopen/LoadLibrary и тестовая .so — EPIC-52 (#21). Hot reload вне объёма (ADR-01).

## 7. Коммиты (каждый сохраняет поведение, CI зелёный)
1. #59a PIC + `snk_target_defaults` + пустой `snk_api.h` · 2. #58a `type_key` · 3. #58b новое хранилище · 4. #61a `ecs_assembler(res)` + prefab, `[[deprecated]]` · 5. #61b importer adapter · 6. #61c render managers, `shader_config::load(res&)`, `configure_pass` · 7. #60a `registration_scope` (+#170) · 8. #60b удалить game_system и мёртвые глобалы · 9. #61d удалить `get_system()` и `p_*`, правило в CLAUDE.md · 10. #62a без `MODULE_PRIORITY`, `loop.init` в `application::run`, состояния · 11. #62b load/unload одного модуля · 12. #62c C-точка входа · 13. #59b `SNK_ENGINE_SHARED`, `SNK_API`, Linux shared в CI, meta_ctx.

## 8. Риски
- LIFO и запрет повторного `construct` могут вскрыть скрытые зависимости порядка — сначала как ASSERT, прогнать редактор.
- `require_shared` держит сервисы дольше хранилища — aliasing `shared_ptr` сохраняет семантику.
- WHOLE_ARCHIVE требует CMake 3.24; разметка `SNK_API` объёмная.
- #58b обязательно до #59b (`std::any`/typeinfo под hidden visibility).

## 9. Решения владельца (2026-10-17: «всё по рекомендации»)
1. Ключ сервиса: свой FNV от `source_location` + опциональный `service_id` → **принято**.
2. Shared: один `snk_engine` SHARED под опцией (OFF), CI: Linux GCC и Windows MSVC (#198), минимум CMake 3.24 → **принято**.
3. `get_system()`: удалить в рамках эпика после короткой `[[deprecated]]`-фазы → **принято**.
4. Объём #62: точка входа + загрузка через указатели на функции сейчас, dlopen и тестовая .so — EPIC-52 → **принято**.
5. Перенос `loop.init` в `application::run` — в #62a; типизированный `frame_context` — отложить до работ по циклу кадра → **принято**.
