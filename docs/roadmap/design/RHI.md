# Дизайн: RHI шаги 1–4, null-бэкенд и POD-пакеты (эпики #13, #15)

**Issue:** #13 (stories #115–#119), #15 (#124, #125, #127; #126 закрыт как выполненный) · **Статус:** утверждено владельцем 2026-10-18 («да, по рекомендациям»; решения — в конце документа)· Основа: `docs/roadmap/ARCHITECTURE_REVIEW.md` §3.5, §9.1, §9.3, §9.4; `CHIMERA_ROADMAP.md` ADR-11; `docs/epics/render-migration/EPIC-45.md`; `STAGE_D_PLAN.md` §1, §5 (null-бэкенд первым, #127 раньше POD, #123 отложен); `PLATFORM_SEAM.md` (swapchain готов в #102); `HEADER_SPLIT.md` (заголовки переезжают в PR-2/PR-4)

## 1. Как устроено сейчас

**Интерфейс драйвера повторяет OpenGL** (`core/core/video/drivers/interface/`):
- Цели рендера — стек: `push_frame_buffer`/`pop_frame_buffer`/`set_render_target(s)` (`rnd_driver_interface.h:160-163`). GL создаёт и удаляет FBO на каждом push/pop, то есть на каждый пасс и камеру (`rnd_gl_driver.cpp:266-287`). Нет begin/end и load/store; очистка — отдельные `clear` (`:168-170`), прямоугольник камеры на backbuffer — через `set_scissor` (`rnd_opaque_pass.cpp:73-80`).
- Состояние глобально изменяемое: `set_render_state` (`rnd_driver_interface.h:183`), PSO нет. Топологии FAN/LOOP/ADJ (`:91-104`), в коде используются только TRIANGLE и LINE.
- Слоты UBO/SSBO зашиты при создании: `create_uniform_buffer(size, binding)` / `create_ssbo_buffer` (`:180-181`). GL сразу делает `glBindBufferRange` (`rnd_gl_uniform_buffer.cpp:12`). `ShaderManager` берёт слоты 0/1 (`rnd_shader_manager.cpp:17-18`); кости — `bind(2)`, индексы скина — `bind(1)` (`rnd_skinning_manager.cpp:60-68`). Эти числа должны совпадать с `layout(binding=)` в GLSL.
- Texture units: `texture_interface::bind(position)` (`rnd_texture_interface.h:107`); `configure_render_pass` привязывает сэмплер i к unit i (`rnd_scene_shader_desc.cpp:104-111`).
- Uniform по имени: `shader_interface::uniform(name, variant)` (`rnd_shader_interface.h:15`). В GL — `glGetUniformLocation` на каждый вызов (`rnd_gl_shader.cpp:15-60`).
- `TEXTURE_USAGE` — одно значение (`rnd_texture_interface.h:22-30`, `:92`), `has_usage` сравнивает через `==` (`:113`). RT композиции создаются как `COLOR_TARGET` (`rnd_transparent_pass.cpp:58`), а потом сэмплятся (`rnd_composition_pass.cpp:96`). Это работает только потому, что GL usage игнорирует. В JSON текстур usage — строка (`rnd_texture_desc.cpp:20-27,182`).
- Инстансинг включается неявно, если в layout есть атрибут MAT4 (`rnd_gl_vertex_array.cpp:95-108`).
- Сырые указатели: в пакетах и сэмплерах лежит `texture_interface*` (`rnd_scene_shader_desc.h:97`). При resize RT удаляется прямо в пассе (`rnd_opaque_pass.cpp:42-46`, `rnd_z_prepass.cpp:34-38`). ImGui получает id через `static_cast<gl::texture*>` (`gui_gl_backend.cpp:75-84`); вызывают это 3 панели редактора (viewport, game view, prefab).
- Swapchain уже есть: `swapchain_interface` (`rnd_swapchain_interface.h`) и `gl::swapchain` поверх `wnd::gl_surface` (#102). `render_init` жёстко создаёт GL-реализацию и падает без `gl_surface` (`rnd_render_service_init.cpp:41-48`). `null_window_system` поверхности не отдаёт (`wnd_window.h:75`).

**Пассы** (`passes/`, порядок — `rnd_render_service_init.cpp:52-57`); каждый идёт по `render_packet_t` (по одному на камеру):
| Пасс | Цели | Что делает |
|---|---|---|
| `z_prepass` | depth D32 (создаёт/ресайзит) | копирует `shader_config` каждого draw и подменяет программу на `z_prepass.*` (`rnd_z_prepass.cpp:88-90`) |
| `opaque_pass` | color RGBA8 + depth из prepass, либо backbuffer (scissor + clear) | `configure_pass` + `bind_skin` + `draw_indices` (`rnd_opaque_pass.cpp:91-105`) |
| `skybox_pass` | color + depth | `packet.skybox_material` (`rnd_skybox_pass.cpp:89-106`) |
| `transparent_pass` | MRT accum/reveal (WBOIT), 2 blend-состояния | `rnd_transparent_pass.cpp:70-120` |
| `debug_pass` | color + depth | свой шейдер мимо `ShaderManager` (`rnd_debug_pass.cpp:142-147,234`), динамический VBO |
| `composition_pass` | color или backbuffer | сэмплирует accum/reveal (units 0/1) (`rnd_composition_pass.cpp:91-98`) |

**Материалы и варианты.** `shader_config` = `constant_data` (теги программы, `defines`, `constants`) + `runtime_data` (сэмплеры, `unordered_map<string, uniform_data>`) (`rnd_scene_shader_desc.h:87-99`).
- Вариант — текстовая вставка `#define` и `#include` при загрузке (`rnd_scene_shader_desc.cpp:7-68`).
- Ключ кеша программ — весь `shader_config` (`rnd_shader_manager.h:89`). Хеш XOR-ит только значения, без имён (`rnd_scene_shader_desc.h:101-113`); равенство — по `cdata` (`:115`).
- `default_material_compiler` в extract зовёт `require_texture` (`rnd_material_compiler.cpp:15`). Он кладёт `uWorldMeshMatr`, `diffuseColor`, `emissiveColor`, `shininess` в map по имени (`:22-27`). Число источников больше не вариант шейдера (#216 убрал `DIRECTION_LIGHT_COUNT`), скиннинг — `USE_ANIMATION` (`:43`). Extract добавляет `OPAQUE` и копирует конфиг для MIX (`scn_render_data_extractor.cpp:125-139`).
- `draw_call_t` несёт `shader_config` по значению и `const std::vector<glm::mat4>* bone_matrices` (указатель в ECS) (`rnd_render_packet.hpp:91-102`); `sort_key` = тот же XOR-хеш.
- Кости — один SSBO на 128 матриц, перезаписывается на **каждый** draw (`rnd_skinning_manager.cpp:19,60-62`).

**Шейдеры** (`core/res/shaders/`):
- Свободные uniform: `first_include.vert:2`, `phong_model_lib.frag:52-54`.
- SSBO `hightlight` на binding 3 никто не привязывает (`phong_model_lib.frag:56`).
- `#version 420` в `sky.vert`/`sky.frag`.
- **Баг std140 исправлен в #216** (PR #231): явные паддинги и `static_assert(offsetof)` в `rnd_shader_manager.h`, `view_position` на 144, шаг `point_light` 80, `GlobalLights` с массивами `MAX_LIGHT_COUNT` и счётчиками `dirLightCount`/`pointLightCount`. Остаётся в R5: точечные источники extractor не заполняет; `MAX_LIGHT_COUNT` дублируется `#define` в `phong_model_lib.frag` без проверки.

**Транзитивность (#212):** `rnd_render_system.h:5-7` включает менеджеры текстур, шейдеров и геометрии. Пассы получают их через `frame_context` (`rnd_frame_context.h:18-25`) и `configure_pass` (`rnd_render_system.h:73-77`).

## 2. Решение

Общее правило: каждый шаг сначала **добавляет** новый вызов рядом со старым, пассы переходят по одному, старый вызов удаляется последним коммитом шага. Все новые сущности — в `rnd::driver` (`snk/driver/`, после #109).

**Шаг 1 — render pass descriptors и usage-флаги (#115).**
- `enum class texture_usage : uint8_t { sampled=1, storage=2, color_target=4, depth_target=8, stencil_target=16 }` через `ds::bit_flags` (уже используется для `clear_flags`, `rnd_driver_interface.h:22`); `has_usage` — побитово.
- JSON текстуры: строка (как сейчас) **или** массив строк; ресейв не нужен.
- `create_texture` валидирует флаги: depth-формат ⇒ `depth_target`, `initial_data` ⇒ `sampled` и так далее. RT пассов получают `color_target|sampled`.
- Пересоздание RT по размеру сейчас скопировано в 4 пассах. Его заменяет `render_targets::ensure(tag, size, format, usage)` в render-слое — шаг к frame graph (§3.5 п.7, вне эпика).
```cpp
enum class load_op : uint8_t { load, clear, dont_care };
enum class store_op : uint8_t { store, dont_care };
struct attachment_desc { texture_handle texture; uint32_t mip = 0, layer = 0; load_op load = load_op::load; store_op store = store_op::store; glm::vec4 clear_color{0}; float clear_depth = 1.f; uint8_t clear_stencil = 0; };
struct render_pass_desc {
	ds::fixed_vector<attachment_desc, 8> colors;      // пусто + backbuffer=true -> окно
	std::optional<attachment_desc> depth_stencil;
	bool backbuffer = false;
	glm::ivec4 render_area{0};  // {l,t,w,h}; ограничивает clear (scissor) и задаёт viewport по умолчанию
	const char* debug_name = nullptr; // метка GPU-профайлера / glPushDebugGroup
};
void begin_render_pass(const render_pass_desc&); void end_render_pass();
```
- До шага 4 `texture_handle` — псевдоним `texture_interface*`; тип меняется в R7.
- GL-реализация хранит кеш FBO: ключ — (id и mip/layer всех attachments), значение — FBO с настроенными draw buffers. При удалении текстуры драйвер выкидывает записи, где она встречается. `clear` превращается в load-op; `dont_care`/`store=dont_care` → `glInvalidateNamedFramebufferData`.
- Вложенных pass нет: validation проверяет это. После миграции из интерфейса удаляются `push/pop_frame_buffer`, `set_render_target(s)`, `clear(...)`, `set_scissor`.

**Шаг 2 — неизменяемый pipeline (#116).**
- `shader_config` сейчас смешивает три вещи. Они разводятся:
  - (a) **идентичность программы** — `shader_variant_key`: теги стадий плюс отсортированные пары (имя, значение) define/constant, стабильный 64-битный хеш имён и значений (§9.4 п.4, взамен XOR). Он появляется уже в R1;
  - (b) **состояние** — то, что сейчас передают в `set_render_state`;
  - (c) **привязки** — `rdata`, переходят в шаг 3.
```cpp
struct pipeline_desc {
	shader_variant_key shader;           // -> программа (кеш ShaderManager)
	vertex_layout      layout;           // из BufferLayout; инстансинг — явный step_rate, не «есть MAT4»
	primitive_topology topology;         // triangle_list, triangle_strip, line_list, line_strip, point_list
	depth_state depth; ds::fixed_vector<blend_state, 8> blend; face_culling_state cull;
	ds::fixed_vector<texture_format, 8> color_formats; texture_format depth_format; // совместимость с pass
};
pipeline_handle create_pipeline(const pipeline_desc&); void bind_pipeline(pipeline_handle);
```
- Render-слой держит `pipeline_cache`: один desc (по хешу и `==`) даёт один handle. Это AC #116.
- Пасс строит desc из «вариант материала + pass-defines (`OPAQUE`, программа `z_prepass`) + своё состояние». Подмена программы в `z_prepass` (`rnd_z_prepass.cpp:88-90`) становится просто другим `pipeline_desc`, без копирования конфига.
- В GL pipeline = {program, состояние}. `bind_pipeline` применяет только разницу с кешем состояния драйвера. После рендера ImGui (stock `imgui_impl_opengl3` меняет состояние GL) нужен `driver::invalidate_state_cache()`.
- VAO в GL остаётся на геометрию; validation проверяет совместимость layout. `set_render_state` удаляется.

**Шаг 3 — bind groups, set/slot, per-draw данные (#117, закрывает #121).**
- Наборы (§9.4):
  - **0** — кадр и вид: `Matrices`, `GlobalLights` (фиксированные массивы и счётчики уже введены в #216);
  - **1** — пасс: accum/reveal для композиции, cubemap неба;
  - **2** — материал: блок `MaterialParams { vec4 diffuseColor; vec4 emissiveColor; float shininess; }` + сэмплеры diffuse/specular/normal;
  - **3** — объект: `ObjectData { mat4 world; uint bone_offset; }` + SSBO `BoneMatrices` (кадровый) + `IndexesBuffer` скина.
```cpp
struct bind_group_layout_desc { ds::fixed_vector<binding_decl, 16> entries; };       // {slot, type: ubo|ssbo|texture, stages}
struct bind_group_desc { bind_group_layout_handle layout; ds::fixed_vector<binding_res, 16> entries; }; // {slot, buffer+offset+size | texture+sampler_desc}
bind_group_handle create_bind_group(const bind_group_desc&, lifetime l = lifetime::persistent); // transient = до конца кадра
void set_bind_group(uint32_t set, bind_group_handle, std::span<const uint32_t> dynamic_offsets = {});
buffer_handle create_buffer(const buffer_desc&); // {size, usage: vertex|index|uniform|storage, update: static|dynamic|per_frame}
```
- Per-draw данные: один кадровый UBO-ring (`per_frame`, тройная буферизация в GL), элементы выровнены по `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`. На draw: `set_bind_group(3, obj_group, {offset})` → `glBindBufferRange`. Это переносится на dynamic UBO в Vulkan и root CBV в D3D12. Push constants не вводим: в GL их нет.
- Кости: экстракт пишет палитру в кадровый `pmr`-буфер пакета. Перед пассами — одна загрузка в кадровый SSBO, в draw — `bone_offset`; лимит 128 на draw снимается.
- Параметры материала переходят из glUniform-по-имени в блок. Компилятор материала выдаёт POD `material_params` (байты std140 по схеме). Инстанс (#125) держит свой UBO-слот и bind group set 2, пишет их только при изменении. `uWorldMeshMatr` уходит в `ObjectData` (`draw_call_t::transform` уже есть, `rnd_render_packet.hpp:98`).
- Схема параметров — явный список `params` в описании шейдера; рефлексия SPIRV-Reflect — с #123 (отложен). `shader_interface::uniform` удаляется.
- Шейдеры: `#version 450 core`, `snk_common.glsl`, макросы привязок. GL гарантирует лишь 8 привязок SSBO, поэтому формула `set*16+slot` для SSBO не работает. Предлагается раскладка по типам ресурса: `SNK_UBO/SNK_SSBO/SNK_TEX(set,slot)`; таблица (set,slot,тип) → binding GL живёт в бэкенде, препроцессор движка разворачивает макрос (Q1). Неиспользуемый `hightlight` удаляется.
- #212 — здесь же: пассы получают `render_context` (интерфейс без менеджеров), `rnd_render_system.h` объявляет менеджеры вперёд.

**Шаг 4 — typed handles (#118).**
- `template<class Tag> struct handle { uint32_t index = 0; uint32_t gen = 0; }`: 8 байт, `index 0` = null. Типы: `texture_handle`, `buffer_handle`, `pipeline_handle`, `bind_group_handle`, `sampler_handle`.
- Таблицы — `ds::handle_pool<T>` (slot map со свободным списком и generation) **внутри бэкенда** (`gl::driver`, `null::driver`). Наружу торчат только handle и `const texture_info& info(texture_handle)`.
- Ресурсы GPU уничтожаются через `destroy(handle)` с отложенным освобождением на N кадров (на будущее для Vulkan; в GL сразу).
- Три уровня (ADR-11):
  - `res::tag` — идентичность ассета, сериализуется;
  - `texture_id` в `texture_manager` — стабильный слот, переживает перезагрузку, resize и заглушку; это и есть «handle-индирекция id → слот» ADR-11;
  - RHI-`texture_handle` — конкретный объект GPU; при пересоздании растёт generation.
- `texture_manager::cache` становится `tag → texture_id`, `texture_id → texture_handle`. Пакеты и материалы держат `texture_id`; bind group пересобирается, когда меняется generation слота.
- Висячий RT после resize ловится по generation, а не через UB.
- ImGui: `gui::renderer_backend::to_imgui(texture_handle) → ImTextureID`. GL-бэкенд резолвит handle в имя GL через внутренний `gl::driver::native_texture` (PRIVATE-доступ по HEADER_SPLIT §6.3). Stock `imgui_impl_opengl3` остаётся; устаревший handle даёт 0.

## 3. Null/validation-бэкенд — первым, на текущем интерфейсе (#119 вперёд)

- `snk::drv_null` (`core/core/video/drivers/impls/null/`): `null::swapchain` (без поверхности, `present` = счётчик кадров) и `null::driver`. Объекты хранят header, размер и данные, если нужны тестам. Шейдер хранит `shader_header` и простые проверки: непустой, `#version` первым.
- `validation_driver` — декоратор над **любым** `driver_interface`: в тестах — над null, в Debug-сборке опционально над GL (`render/validate=true`).
- Выбор: `render/backend = gl|null`. `render_init` берёт null, если ключ задан или у окна нет `gl_surface` (тогда `null_window_system` работает без исключения).
- **Что валидирует (сейчас):**
  - баланс push/pop за кадр и стек = 1 в `present`;
  - `set_render_target` только после push;
  - usage attachments, одинаковые размеры attachments;
  - viewport и scissor внутри цели;
  - `draw` только после `use()`, `count+base_index` ≤ размера IBO, наличие layout у VAO;
  - `set_data` UBO/SSBO в пределах размера;
  - обращения к удалённым объектам (реестр живых объектов);
  - бюджет: число FBO, программ, текстур за кадр.
- **Что добавится по шагам:**
  - begin/end парны, без вложенности;
  - pipeline ↔ форматы pass;
  - все set'ы, объявленные layout'ом pipeline, привязаны до draw; совпадают типы и usage `sampled`;
  - выравнивание dynamic offset;
  - generation handle.
- **Трасса команд**: кольцевой журнал (`begin_pass(name, attachments)`, `bind_pipeline`, `set_bind_group`, `draw(count)`). Тесты проверяют инварианты, а не байтовую трассу: «на smoke_level N draw в opaque», «composition сэмплирует accum/reveal», «ни одного FBO на кадр после R3».
- **Тесты**:
  - контрактные `rnd_null_driver_tests.cpp` и `rnd_validation_tests.cpp`: заведомо неверные вызовы дают ошибку;
  - кадр движка, как в `app_engine_without_editor_tests.cpp`: `null_window_system` + `drv_null`, загрузка `levels/smoke_level.desc`, 3 кадра, ноль ошибок валидации;
  - бенчмарк extract для #15 без GPU-шума.
- **Чего не ловит:**
  - компиляцию и линковку GLSL, несовпадение имён и блоков в шейдере (до #123);
  - пиксели, std140-смещения, блендинг;
  - утечки GL-состояния (ImGui);
  - ошибки драйвера и Mesa, синхронизацию и производительность GPU.
  Это остаётся за визуальным smoke.

## 4. Место #15 (POD-пакеты, материал-инстансы)

1. **#127 первым** (решение владельца, US-45-1/2/3). Работает на текущем интерфейсе:
   - стабильный `shader_variant_key`;
   - `shader_config_cache::intern(cdata) → variant_id`, `draw_call_t::material` → `variant_id` + ссылка на кешированный материал (сэмплеры и параметры);
   - кеш материала на entity с dirty-флагом на `material_desc_component`;
   - `OPAQUE` — pass-define, а не мутация конфига в extract.
   Цель: `Extract.ShaderDesc` < 0.5 мс, `Extract.PushDraws` < 0.5 мс на 79 мешей.
2. **#124**: `draw_call_t = { u64 sort_key; geometry_id; material_instance_id; u32 indices_count, vx_begin, ind_begin; mat4 transform; skin_id; u32 bone_offset, bone_count; }` — POD, `std::is_trivially_copyable` в `static_assert`.
   - `skybox_material` → `material_instance_id`.
   - Кости копируются в `pmr`-вектор пакета (кадровый аллокатор; правило «не хранить через кадр» соблюдается).
   - `sort_key` = (пасс, pipeline, материал, глубина); есть тест сортировки.
3. **#125**: `material_instance` — handle в `material_instance_cache` (render-слой). Хранит: базовый вариант, `material_params`, `texture_id[]`, после R5 — bind group set 2. Пересобирается при изменении desc или по сигналу hot reload `res`; `require_texture` уходит из extract в компиляцию инстанса.
4. **Замер** (критерий EPIC-45: `ExtractRenderData` < 2 мс на 100 мешей).
   - Сцена `levels/bench_100_meshes.desc` (100 мешей, 10 со скиннингом, 2 камеры).
   - Тест или режим `--bench-frames 300` на null-бэкенде: среднее `profiler_last_frame_stats()` по `ExtractRenderData` и `Extract.*` после 30 кадров прогрева, вывод в лог/JSON.
   - Baseline снимается в R0, повтор — после R1 и R6; job в CI — отчётный, не блокирующий (Q5).
   - Реализация (R0c): `unit_tests --run_test=AppEngineNullBackendBench/ExtractRenderDataOnHundredMeshes` (отключён по умолчанию, `tests/app_engine_null_backend_bench.cpp`), 30 кадров прогрева и 300 измеряемых, суммы зоны за кадр; сцена — 10 роботов со скиннингом и 90 одноквадовых мешей из `bench/bench_quad.desc`, 2 камеры; job «Extract benchmark (Linux, report-only)», артефакт `extract-bench` (JSON).
   - **Baseline R0** (GCC Release, ubuntu-latest, null-бэкенд, мс за кадр; 2 камеры × 100 мешей = 200 вызовов на зону):

     | Зона | mean | p50 | p95 | max |
     |---|---:|---:|---:|---:|
     | `ExtractRenderData` | 0.201 | 0.197 | 0.222 | 0.281 |
     | `Extract.ShaderDesc` | 0.111 | 0.108 | 0.125 | 0.151 |
     | `Extract.PushDraws` | 0.030 | 0.029 | 0.033 | 0.103 |
     | `Extract.BuildDC` | 0.008 | 0.008 | 0.008 | 0.033 |
     | `Extract.Skinning` | 0.008 | 0.008 | 0.008 | 0.022 |

     Критерий EPIC-45 (< 2 мс на 100 мешей) на этом baseline выполнен с большим запасом; цели `Extract.ShaderDesc`/`Extract.PushDraws` из п. 1 (< 0.5 мс на 79 мешей) тоже. Раннеры шумные: сравнивать порядок величин, не проценты.

## 5. План по PR (после PR-5 #109 и #174; волны — `STAGE_D_PLAN.md` §2)

| PR | Issue | Содержимое | Пассы |
|---|---|---|---|
| R0 | #119 (часть) | `drv_null`, `validation_driver`, `render/backend`, тесты, baseline-бенчмарк, CI-grep «GL вне `drv_*`» с allow-list (`impl_glfw` hints), эталонные PNG | — |
| R1 | #127 | variant key, кеш конфигов и материалов, pass-define `OPAQUE` | все (только `configure_pass`) |
| R2 | #115a | usage-флаги, JSON-совместимость, `render_targets::ensure` | все RT-пассы |
| R3 | #115b | `begin/end_render_pass`, кеш FBO; удаление push/pop/clear/scissor | z_prepass+opaque → skybox+debug → transparent+composition |
| R4 | #116 | `pipeline_desc`/кеш, трим топологий, debug_pass через кеш; удаление `set_render_state` | все |
| R5 | #117 (+#121, #212) | блоки, `SNK_*`, ring UBO, кадровый SSBO костей, bind groups, `MaterialParams`, lights count; удаление `uniform()` | все, затем GUI |
| R6 | #124, #125 | POD `draw_call_t`, `material_instance`, повторный замер | extract + все |
| R7 | #118 | handle pools, `texture_id`, `to_imgui`, generation в validation; закрытие #119 (критерий «вне `drv_*` нет GL») | все + editor |

- **Неизменность рендера** на каждом PR:
  - (1) `editor-smoke` (`build.yml:311-377`, `check_smoke.sh`) без изменений;
  - (2) новая проверка `tools/headless/compare_shots.sh`: эталоны `smoke`/`game_view`/`checklist` (llvmpipe), `compare -metric AE -fuzz 2%` ниже порога;
  - (3) кадр на null без ошибок валидации;
  - (4) в Debug-сборке validation поверх GL в smoke-прогоне.
  R5 меняет раскладку блоков и шейдеры — эталон обновляется осознанно одним коммитом с объяснением.
- **#109:** RHI-код только после PR-5, пути — уже `snk/driver/`, `snk/render/`. **#120/#122** делят `rnd_shader_manager`/`rnd_gl_shader` — не параллелить с R4/R5. **#212** закрывается в R5 (иначе заголовки правятся дважды).

## 6. Риски и вопросы владельцу

**Риски:**
- (a) R3/R5 трогают все пассы и `gui_gl_backend` — нужна одна ветка «render» волны 2;
- (b) кеш FBO и удаление текстур — держать инвалидацию строго в драйвере;
- (c) утечка состояния между ImGui и движком после кеша состояния GL;
- (d) лимиты GL (8 SSBO, выравнивание UBO 256) и различия llvmpipe и драйверов Windows;
- (e) эталонные PNG хрупки при правках сцены;
- (f) `ImTextureID` 8 байт — проверить версию ImGui (`ImU64` по умолчанию в 1.91+);
- (g) R6 меняет экстрактор сцены (`scn_render_data_extractor.cpp`) — пересекается с #12 (world_context).

**Вопросы (≤6):**
1. Привязки GL: по типам ресурса (`SNK_UBO/SSBO/TEX`, таблица в бэкенде) вместо `set*16+slot` из §9.4? **Рекомендация: да**, §9.4 поправить.
2. Камера — в set 0 («кадр + вид»), set 1 — входы пасса? **Рекомендация: да.**
3. Схема параметров материала — явный список в описании шейдера до #123? **Рекомендация: да.**
4. Эталонные PNG — в репо, обновлять отдельным коммитом? **Рекомендация: да** (пересборка базы в CI вдвое дороже).
5. Перф-job бенчмарка — только отчёт, не гейт? **Рекомендация: да**, на раннерах шумно.
6. Разделить #119: null/validation сейчас (R0), критерий «вне `drv_*` нет GL» — в R7? **Рекомендация: да.**

## Решения владельца (2026-10-18, «по рекомендациям»)

1. Привязки GL раскладываются по типам ресурса (`SNK_UBO/SNK_SSBO/SNK_TEX(set,slot)`, таблица в бэкенде); формула `set*16+slot` из ARCHITECTURE_REVIEW §9.4 поправляется.
2. Камера — в set 0 («кадр + вид»), set 1 — входы пасса.
3. Схема параметров материала — явный список `params` в описании шейдера до #123.
4. Эталонные PNG хранятся в репо и обновляются осознанно отдельным коммитом.
5. Перф-джоб бенчмарка — только отчёт, не гейт CI.
6. #119 делится: null/validation-бэкенд — сейчас (R0), критерий «вне `drv_*` нет GL» — в R7.
