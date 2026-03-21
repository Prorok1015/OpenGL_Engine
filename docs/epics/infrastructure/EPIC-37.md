# EPIC-37: Unit Test Coverage — аудит, исправления, расширение

**Status:** done
**Theme:** infrastructure
**Dependencies:** none

---

## Мотивация

Текущее покрытие тестами ~15-20% по подсистемам. 67 тест-кейсов в 13 файлах.
Ряд тестов содержит баги, один тест flaky, другой — монолитный.
Критические подсистемы (ресурсная система, рендер-пайплайн, ECS core, конфиг) не покрыты вообще.

### Текущее состояние покрытия

| Подсистема | Файл тестов | Кейсов | Качество | Заметки |
|------------|-------------|--------|----------|---------|
| `ds::bit_flags` | `ds_bit_frags_tests.cpp` | 11 | Отлично | Полное покрытие операторов |
| `ds::Event` | `ds_event_tests.cpp` | 8 | Хорошо | simple + managed policies |
| `ds::app_data_storage` | `ds_store_tests.cpp` | 2 | Хорошо | basic + hierarchical |
| `ds::rtree_q` | `ds_rtree_tests.cpp` | 3 | Хорошо | build/insert/query |
| `ds::fixed_vector` | `ds_fixed_vector_test.cpp` | 1 | Слабо | Один монолитный тест |
| `ds::polymorphic_cast` | `ds_polymorphic_cast_tests.cpp` | 2 | Плохо | 3 теста отключены из-за бага |
| `Timer` | `timer_tests.cpp` | 1 | Плохо | Flaky busy-loop |
| `eng::transform3d` | `transform_3d_tests.cpp` | 5 | Отлично | epsilon-сравнения |
| Transform system | `transform_system_tests.cpp` | 2 | Хорошо | Иерархия глубин + world matrix |
| `scn::*_desc` (6 типов) | `scn_desc_prefab_tests.cpp` | 22 | Отлично | Assembly в ECS |
| Editor panels | `edt_panel_tests.cpp` | 13 | Слабо | Smoke-тесты "не крашится" |
| Editor inspector/console | `edt_inspector_console_tests.cpp` | 14 | Слабо | Smoke-тесты |

### Непокрытые критические подсистемы

| Подсистема | Файлов | Тестируемо без GPU |
|------------|--------|-------------------|
| Resource system (tag, VFS, cache, adapters) | ~20 | Да (tag, control_block, text_adapter, path_mapper) |
| Render data structures (packets, lights, shader_config, buffer_layout) | ~15 | Да (pure data + conversion) |
| ECS core (command_buffer, invoker, traits) | ~10 | Частично |
| Config system (cfg_api) | ~5 | Да |
| Input system | ~10 | Частично |

## User Stories

### US-37-1: Исправить баг в ds_polymorphic_cast и включить тесты
**Файлы:** `core/core/common/ds/ds_polymorphic_cast.hpp`, `unittests/ds_polymorphic_cast_tests.cpp`

Баг: unique_ptr overload (строка 58) имеет `ASSERT_MSG(ptr1 != ptr2, ...)` — условие инвертировано (должно быть `==`). Строка 59 использует `std::static_pointer_cast` с `unique_ptr` — не компилируется.

**AC:**
- [x] Исправлен assert: `ptr1 != ptr2` → `ptr1 == ptr2`
- [x] Заменён `std::static_pointer_cast` на корректный release/reset для unique_ptr
- [x] Раскомментированы и проходят тесты unique_ptr (UniquePointerSuccess, UniquePointerOwnershipTransferred)
- [x] Добавлен тест RawReferenceSuccess

### US-37-2: Разбить ds_fixed_vector_test на отдельные кейсы
**Файлы:** `unittests/ds_fixed_vector_test.cpp`

**AC:**
- [x] Каждая операция в отдельном BOOST_AUTO_TEST_CASE (16 кейсов)
- [x] Добавлены тесты: emplace_back, iterator, front/back, at bounds, insert, erase, copy/move, equality
- [x] Тесты overflow: InitializerListOverflow_Throws, PushBack_WhenFull_Throws

### US-37-3: Исправить timer_tests — убрать flaky busy-loop
**Файлы:** `unittests/timer_tests.cpp`

**AC:**
- [x] Убран busy-loop, используется sleep_for(50ms)
- [x] Тесты детерминистичные: Now_ReturnsPositiveValue, Now_IsMonotonicallyIncreasing, Now_MeasuresElapsedTime
- [x] Добавлены тесты NowSec_ReturnsPositiveValue, NowSec_IsMonotonicallyIncreasing

### US-37-4: Тесты ресурсной системы — res::tag
**Файлы:** `unittests/res_tag_tests.cpp` (новый)

**AC:**
- [ ] Парсинг: `tag::make("path/file.txt")` → корректные protocol/path/name/extension
- [ ] Парсинг с протоколом: `tag::make("memory://data")` → protocol = "memory"
- [ ] Сравнение: одинаковые теги равны, разные — не равны
- [ ] Хеширование: одинаковые теги дают одинаковый хеш
- [ ] Извлечение: `.extension()`, `.pure_name()`, `.relative()`
- [ ] Невалидный тег: пустая строка, только протокол

### US-37-5: Тесты ресурсной системы — control_block и text_adapter
**Файлы:** `unittests/res_control_block_tests.cpp` (новый), `unittests/res_text_adapter_tests.cpp` (новый)

**AC:**
- [x] control_block: статусы pending→ready, callback вызывается при ready (11 кейсов)
- [x] control_block: error status, deferred callbacks, thread safety
- [x] text_adapter: deserialize bytes → text_resource с корректным содержимым (9 кейсов)
- [x] text_adapter: serialize roundtrip, empty content, adapter_info

### US-37-6: Тесты рендер-пайплайна — buffer_layout
**Файлы:** `unittests/rnd_buffer_layout_tests.cpp` (новый)

**AC:**
- [ ] shader_data_type_size() возвращает корректные размеры для всех 11 типов
- [ ] get_component_count() корректен для каждого типа
- [ ] BufferLayout вычисляет stride как сумму размеров элементов
- [ ] BufferLayout вычисляет offset каждого элемента последовательно
- [ ] Поиск элемента по имени работает

### US-37-7: Тесты рендер-пайплайна — light_data и shader_config
**Файлы:** `unittests/rnd_light_data_tests.cpp` (новый), `unittests/rnd_shader_config_tests.cpp` (новый)

**AC:**
- [ ] scene_lights_t::to_gpu_params() корректно конвертирует 0, 1, MAX_LIGHT_COUNT источников
- [ ] to_gpu_params() обрезает при превышении MAX_LIGHT_COUNT
- [ ] shader_config: одинаковые конфиги дают одинаковый хеш
- [ ] shader_config: разные шейдеры дают разный хеш
- [ ] shader_program_data builder: set/get vertex/fragment/geometry shader по расширению

### US-37-8: Тесты рендер-пайплайна — render_mode conversion и geometry_desc serialization
**Файлы:** `unittests/rnd_state_helper_tests.cpp` (новый), `unittests/rnd_geometry_desc_tests.cpp` (новый)

**AC:**
- [ ] to_string() и render_mode_from_string() для всех 12 режимов
- [ ] Roundtrip: enum → string → enum для каждого режима
- [ ] shader_data_type_to_string / from_string roundtrip для всех типов
- [ ] geometry_desc JSON serialization roundtrip

---

## Порядок выполнения

```
US-37-1 (polymorphic_cast fix)  ─┐
US-37-2 (fixed_vector split)    ─┼─ параллельно, независимы
US-37-3 (timer fix)             ─┘
         │
         ▼
US-37-4 (res::tag tests)       ─┐
US-37-5 (control_block + text)  ─┼─ параллельно, независимы
US-37-6 (buffer_layout tests)  ─┘
         │
         ▼
US-37-7 (light_data + shader)  ─┐
US-37-8 (render_mode + geom)   ─┘─ параллельно
```

## Риски

- Некоторые хедеры рендера могут транзитивно тянуть OpenGL зависимости → обойти через forward declarations или условную компиляцию
- `res_control_block` использует threading → тесты должны быть детерминистичными, без гонок

## Критерии завершения эпика

- [x] Все существующие тесты исправлены и проходят (US-37-1, 2, 3)
- [x] Ресурсная система покрыта тестами: tag, control_block, text_adapter (US-37-4, US-37-5)
- [x] Рендер data structures покрыты: buffer_layout, light_data, shader_config, render_mode (US-37-6, 7, 8)
- [x] Общее количество тест-кейсов > 120 (было 67, стало 161)
- [x] Все тесты проходят: `unit_tests.exe` — 0 failures
