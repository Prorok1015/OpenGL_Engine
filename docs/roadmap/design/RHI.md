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
- **Сделано в R3** (#115, часть b): `render_pass_desc` — `snk/driver/rnd_render_pass.h`, кеш FBO — `snk/driver/rnd_framebuffer_cache.h` (общий для GL и null, ключ — идентичность текстуры + mip/layer каждого attachment). Уточнения к схеме выше:
  - окно: `backbuffer = true`; attachment с нулевой `texture` задаёт load/store цвета окна (`colors[0]`) и его глубины (`depth_stencil`); пустой список — просто load. Нулевая `texture` в обычном pass — неиспользуемый слот draw buffer;
  - `render_area` — только ограничение clear (scissor на время очистки, потом выключается) и viewport по умолчанию; для offscreen-целей `{0}` очищает всю текстуру, как раньше;
  - clear глубины не зависит от маски записи глубины в последнем `set_render_state` (драйвер её помнит);
  - текстура при удалении сама выбрасывает свои FBO из кеша (общий `framebuffer_registry`, живёт дольше драйвера);
  - validation: бюджет `max_frame_buffers` считает pass за кадр (раньше push), кадр на null-бэкенде не строит FBO после прогрева (`SmokeLevelBuildsNoFramebufferAfterWarmUp`).

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
- **Сделано в R4** (#116) — `snk/driver/rnd_pipeline.h`, `rnd_pipeline_cache.h`; отличия от схемы выше:
  - кеш живёт в драйвере (общий шаблон для GL и null, validation пересылает), а не в render-слое: один `pipeline_desc` (по хешу и `==`) даёт один `pipeline_handle`; описание копируется при создании, `pipeline_info(handle)` отдаёт только `const`. Пассы держат `rnd::pass_pipelines` (кадровая память): вариант материала + состояние пасса → handle без повторного хеширования на каждый draw;
  - `pipeline_desc` = `shader` (`shader_ref`: объект и его serial, чтобы новый шейдер по тому же адресу не принимался за старый), `topology`, `depth`, `cull`, `blend`, `color_formats`, `depth_format`. Нет `layout`: VAO по-прежнему несёт раскладку, инстансинг остаётся неявным (нужны раскладки геометрии в пассах, это не R4). `depth_format` — `optional`: пасс может не иметь глубины; пустой слот цвета (null texture) — пустой `optional` в `color_formats`;
  - топологии урезаны до `TRIANGLE`, `TRIANGLE_STRIP`, `LINE`, `LINE_STRIP`, `POINT`; `draw_indices` больше не принимает режим, его задаёт привязанный pipeline. Хак «`LINE_STRIP_ADJ` = каркас» и `glPolygonMode` на каждом draw удалены (полигоны всегда `GL_FILL`);
  - GL хранит кеш того, что записано в API по полям (программа, depth test/func/mask, GL_BLEND и функции по целям, culling) и пишет только разницу. `invalidate_state_cache()` вызывается в начале каждого `render_frame`: ImGui между кадрами оставляет своё состояние (в том числе `glBlendFunc` на всех целях). Маска глубины для clear в `begin_render_pass` (`m_depth_write`) берётся из последнего привязанного pipeline и инвалидацией не сбрасывается — ImGui её не трогает;
  - `bind_pipeline` идёт после `begin_render_pass` (форматы pass известны), раньше `set_render_state` вызывался до него. Пиксели не меняются: clear глубины и так принудительно открывает маску;
  - `shader_interface::use()` удалён: программу делает текущей `bind_pipeline`. Draw с нерабочей программой пропускается (раньше рисовался чужой программой);
  - validation: `draw_without_use` заменён на `draw_without_pipeline` (привязка действует до конца pass), добавлены `bind_pipeline_outside_pass`, `invalid_pipeline` (нет шейдера, неизвестный handle, blend больше целей), `pipeline_format_mismatch` (форматы цвета и глубины против pass; окно — `backbuffer_color_format`/`backbuffer_depth_format`), `budget_pipelines` (новые pipeline за кадр), `dead_object` для шейдера pipeline.

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
- **R5 разбит на два PR.** R5a (`refs #117`): API bind groups и буферов по handle, `SNK_*`, наборы 0, 1, 3, кольцо `ObjectData`, кадровый SSBO костей. R5b: `MaterialParams` и набор 2, удаление `shader_interface::uniform`, закрытие #121 и #212.
- **Сделано в R5a** (#117) — `snk/driver/rnd_bind_group.h`, `rnd_resource_tables.h`, `snk/render/rnd_bindings.h`, `rnd_per_draw_data.h`; отличия от схемы выше:
  - API: `buffer_desc{size, usage, update, debug_name}` и `create_buffer(desc)` / `destroy_buffer` / `update_buffer(handle, offset, bytes)` (старый `create_buffer()` без аргументов остаётся для вершин и индексов); `create_bind_group_layout` (один набор записей — один handle), `create_bind_group(desc, lifetime)`, `destroy_bind_group`, `set_bind_group(set, group, dynamic_offsets)`; `begin_frame()` (его зовёт `render_system::render_frame` перед пассами), `bindings()` (таблица бэкенда), `uniform_offset_alignment()` (GL спрашивает один раз). `binding_res` несёт `type`: пара `(type, slot)` называет привязку, потому что слоты считаются по типам ресурса. `uniform_buffer_interface` и `ssbo_buffer_interface` удалены вместе с `create_uniform_buffer` и `create_ssbo_buffer`;
  - таблица (set, slot, тип) → привязка GL — `per_type_binding_table` (`binding_table::resolve`), единая для GL и null: UBO по 2 на набор (0–7), SSBO только в наборе 3 (0–1), текстуры: набор 1 — 4 (units 0–3), набор 2 — 8 (units 4–11). Для SSBO занято 2 из 8 гарантированных привязок. Макросы `SNK_UBO/SNK_SSBO/SNK_TEX(set, slot)` разворачивает `rnd::expand_binding_macros` при загрузке шейдера (`layout(std140|std430, binding = N)`, `layout(binding = N)`); макрос вне таблицы — ошибка в логе `shader/load`. Препроцессор добавляет `#define MAX_LIGHT_COUNT` (из `lights_params::MAX_LIGHT_COUNT`, единственный источник; `snk_common.glsl` проверяет его `#error`) и define стадии `SNK_VERTEX_SHADER`/`SNK_FRAGMENT_SHADER`/`SNK_GEOMETRY_SHADER`;
  - `pipeline_desc::set_layouts` — раскладка группы на каждом наборе, который читает программа (входит в хеш). Draw без группы нужной раскладки на таком наборе — нарушение `draw_missing_bind_group`/`bind_group_layout_mismatch`. Привязки групп действуют до конца render pass. Transient-группа живёт до следующего `begin_frame`;
  - набор 0 — два буфера `dynamic` (`Matrices`, `GlobalLights`), одна persistent-группа в `ShaderManager`; пассы пишут данные камеры перед своими draw, как и раньше. Набор 1 — transient-группа текстур в композиции (accum, reveal) и в skybox (кубмапа первым сэмплером материала; пока кубмапа не загружена, небо не рисуется);
  - набор 3 — `rnd::per_draw_data`: кольцо `ObjectData` (`per_frame`, `frames_in_flight` = 3 копии в драйвере; шаг — размер, округлённый до выравнивания UBO), SSBO `BoneMatrices` (`per_frame`, растёт степенью двойки, лимит 128 снят), SSBO `IndexesBuffer` на скин и пустая заглушка для остальных draw. Группа одна на скин, динамическое смещение выбирает элемент кольца. Перед пассами `frame_upload_pass` собирает `ObjectData` всех пакетов (кадровая память, `reserve`) и пишет кольцо и кости по одному `update_buffer`; кольцо и кости растут и пересоздают группы только когда кадр не помещается;
  - кости: экстрактор копирует палитру скелета в `render_packet_t::bones` (кадровая память, один раз на скелет по адресу источника, `reserve` по сумме матриц сцены), draw хранит `bone_offset`; `draw_call_t::bone_matrices` (указатель в ECS) удалён. Шейдер читает `bones[boneOffset + index]`. `skinning_manager` больше не владеет GPU-ресурсами: отдаёт только `build_indices(skin)`;
  - `uWorldMeshMatr` и `first_include.vert` удалены, мировая матрица идёт в `ObjectData`; SSBO `hightlight` и ветка `HIGHTLIGHT_TRIANGLES` удалены; все шейдеры — `#version 450 core`;
  - validation: новые правила `invalid_buffer`, `bind_group_outside_pass`, `invalid_bind_group`, `bind_group_layout_mismatch`, `binding_outside_table`, `transient_group_expired`, `dynamic_offset_alignment`, `draw_missing_bind_group`, `budget_bind_groups` (только persistent-группы); убраны `object_kind::uniform_buffer/ssbo_buffer` и `trace_op::bind_ssbo`. null: `bound(set)`, `bind_log()` (по `record_bindings`), `draws_with_set()`, `buffer_bytes(handle, copy)`, `shader_sources()` (по `record_shader_sources`), выравнивание UBO 256.
- **Сделано в R5b** (#117, #121, #212, #283) — `snk/render/shader/rnd_material_params.h`, `snk/render/rnd_material_groups.h`, `snk/render/rnd_render_context.h`, `snk/render/rnd_gpu_blocks.h`; отличия от схемы выше:
  - набор 2: `MaterialParams` (`SNK_UBO(2, 0)`, 48 байт: `diffuseColor`, `emissiveColor`, `shininess`) и три карты `SNK_TEX(2, 0..2)` — diffuse, specular, normal (units 4–6 таблицы). Схема — явный список `phong_params` из `param_decl{имя, тип, offset}`; рядом C++-структура `material_params` со `static_assert` на смещения, `is_std140_schema` проверяет раскладку std140 на этапе компиляции. `default_material_compiler` собирает `named_param` в байты (`pack_params`), результат лежит в `shader_config::runtime_data::material_params`, а не в `unordered_map` униформ. Рефлексии нет (#123);
  - `rnd::material_groups` (принадлежит `render_system`): на каждый `compiled_material` один буфер `MaterialParams` (`static_data`, пишется один раз) и одна persistent-группа. Ключ — `compiled_material::serial`, он не повторяется, в отличие от адреса. Экстрактор пересобирает материал при `dirty` и при смене `texture_manager::generation()` (hot reload текстуры, ресайз RT): новый `serial` — новая группа. Группа материала, которого не было в предыдущем кадре, уничтожается. Карта, которой нет у материала (или она ещё грузится), — текстура-заглушка 1x1. Скайбокс набор 2 не читает; `set_layouts::scene()` называет раскладку набора 2 для opaque, transparent и z_prepass. `material_instance` (#125) получит из этого свою группу и буфер;
  - удалены `shader_interface::uniform`, `uniform_data`, `configure_render_pass`/`configure_uniforms`, `ShaderManager::apply*`. Свободных uniform в `core/res/shaders` нет (тест `ShippedShaderSources`), у GUI-бэкенда собственных шейдеров нет (stock ImGui);
  - #212: пассы получают `rnd::render_context` — пакеты, текстуры (`find_texture`, `ensure_render_target`), `geometry`, `variants`/`program`, `layouts`, `write_view`/`write_lights`/`bind_frame`, `bind_material`, `upload_objects`/`bind_object`. Реализацию `render_frame` строит на кадр; менеджеры за ней не видны, `configure_pass(render_context&, material)`. `frame_context` остался стадии extract: `data`, `textures`, `variants`, `material_compiler`, `backbuffer_size`; поля `shaders`, `geometry`, `skinning`, `draw_data` убраны. `rnd_render_system.h` объявляет менеджеры вперёд и держит их в `unique_ptr`. `ShaderManager`, `geom_manager` и `pass_pipelines` — внутренние заголовки render и в SDK не ставятся; `global_params`/`lights_params` переехали в `rnd_gpu_blocks.h`, `set_layouts` — в `rnd_bindings.h`. `texture_manager` остаётся публичным: его API нужен редактору и сцене, заголовок подключается явно;
  - #283: парсер макросов пропускает `/* */`; неизвестный вызов `SNK_*(...)` и любая ошибка макроса — ошибка `shader_config::load` (`load_result::error`), шейдер не доходит до драйвера, `ShaderManager` не пробует его повторно. Define-ы вставляются после строки `#version`, которую ищет `insert_after_version` с учётом комментариев; без `#version` шейдер не грузится. Кеш skin: `skinning_manager::version()`/`generation()`, повторный `register_weights` (hot reload) пересоздаёт буфер и группу, незарегистрированный skin запоминается как «нет», `build_indices` не зовётся на каждый draw; `skinning_lib.glsl` проверяет `data.length()` до чтения. Validation: правило `binding_offset_alignment` — статический `offset` UBO кратен `uniform_offset_alignment()`, SSBO — новому `storage_offset_alignment()` драйвера. Удалены `scn::hightlight_component` и шаблоны `transparent.vert`, `mix_opaque_trans_scene.vert` (`LEGACY_TEMPLATES.md`).
- **R6 разбит на два PR.** R6a (`refs #124`): POD `draw_call_t` и сортировка. R6b: `material_instance` + `material_instance_cache` (#125) и повторный замер.
- **Сделано в R6a** (#124) — `snk/render/rnd_render_packet.hpp`; отличия от схемы выше:
  - `draw_call_t` — POD (`static_assert` на `is_trivially_copyable`, `is_trivially_destructible`, `is_standard_layout`): `sort_key`, `geometry` (`geometry_id`), `variant` (`variant_id`, это pipeline), `material` (`material_instance_id`), `indices_count`, `vx_begin`, `ind_begin`, `transform`, `skin` (`skin_id`), `bone_offset`, `bone_count`. Ни тегов, ни указателей. Идентификаторы — `draw_ref<Table>`, индекс в таблице пакета;
  - таблицы живут в `render_packet_t` на кадровой памяти (`reserve` в экстракторе): `geometries` и `skins` (`res::tag`), `materials` (`const compiled_material*`, неосновной указатель — владеют `compiled_material_component` и список retired экстрактора, как и раньше). Разворачивают id методы пакета `geometry_of`, `skin_of`, `material_of`; пассы берут их оттуда. Каждый различный тег или материал попадает в таблицу один раз. Указатели на кадровую память через кадр не хранятся: пакет умирает вместе с кадром;
  - `material_instance_id` пока — индекс в `render_packet_t::materials`, то есть тот же `compiled_material`, что и после R5b (его `serial` по-прежнему ключ `material_groups`). В R6b таблица заменяется `material_instance_cache`, id остаётся ручкой. `skybox_draw_t` не менялся: одиночный объект пакета, не draw;
  - `sort_key` (`make_sort_key`, 64 бита). Opaque: `pass(2) | pipeline(20) | material(22) | depth(20)`, ближние первыми. Transparent: `pass(2) | depth(20) | pipeline(20) | material(22)`, дальние первыми — порядок смешивания важнее группировки. `depth` — расстояние вдоль оси камеры в пространстве вида, квантованное верхними 20 битами `float` (расстояния неотрицательны, порядок битов совпадает с порядком значений; позади камеры и NaN дают 0). `pipeline` — `variant_id::value`, `material` — индекс в таблице пакета (важна группировка, а не само число);
  - сортировка — `sort_draws` (`std::stable_sort`: равные ключи остаются в порядке извлечения) в конце извлечения пакета, до `frame_upload_pass`: индекс в кольце `ObjectData` — позиция в отсортированном списке, поэтому `object_base` и `opaque_object(i)` не менялись. Прежний `sort_key` (хеш варианта) никто не читал;
  - вывод: opaque рисуются с depth-тестом (z-prepass + `LEQUAL`), порядок меняет картинку только у копланарных поверхностей; transparent — WBOIT, где порядок и так слабо влияет, но дальше-первым он теперь определён ключом, а не порядком сущностей. Эталон visual smoke должен остаться 0.000%;
  - тесты: `rnd_draw_call_tests` (POD, порядок ключа: пасс, pipeline, материал, глубина; transparent дальние первыми; устойчивость), `app_engine_null_backend_draw_packet_tests` (на кадре null-бэкенда пакеты содержат ровно те же draw, что сцена, отсортированы, id разворачиваются в своём пакете, transparent дальше-первым); существующие проверки `ObjectData` (`app_engine_null_backend_bind_group_tests`) проходят по отсортированным draw.
- **Сделано в R6b** (#125) — `snk/render/rnd_material_instance_cache.h`; отличия от схемы выше:
  - `rnd::material_instance_cache` (принадлежит `render_system`, в `frame_context::materials`): слоты по ключу `(материал-desc, skinned)`, `material_instance_id` — индекс слота. Инстанс — `compiled_material` (варианты `base`/`opaque`, `material_params`, сэмплеры), `serial` по-прежнему ключ для `material_groups`. Таблица `render_packet_t::materials` и `material_of` удалены; пассы получают инстанс из `render_context::material(id)`, `bind_material(id)`. `skybox_draw_t::material` — тоже `material_instance_id`;
  - `scn::compiled_material_component` и список retired у экстрактора удалены: экстрактор делает `acquire(desc, has_skinning, dirty)` и берёт `base`/`opaque` инстанса. `require_texture` вызывается только компиляцией инстанса (`build_compiled_material`), экстрактор не включает `rnd_texture_manager.h`;
  - пересборка на месте (id остаётся): `dirty` у `material_desc_component` (меняет все инстансы desc, статический и скиннинговый, один раз за кадр), смена `texture_manager::generation()` (hot reload, ресайз RT), карта ещё грузилась (повтор раз в кадр). Другой объект desc — другой ключ. Инстанс, который не спрашивали `grace_frames` (8) кадров, освобождается, слот переиспользуется;
  - отличие от схемы: bind group набора 2 и буфер `MaterialParams` по-прежнему принадлежат `material_groups` и ключуются содержимым (равные инстансы делят группу), а не живут в инстансе: это уже снимает лишние группы, и перенос в инстанс сломал бы это разделение;
  - тесты: `rnd_material_instance_cache_tests` (попадание в кеш, ключи, пересборка по desc, по generation, повтор для грузящейся карты, вытеснение), `app_engine_null_backend_material_cache_tests` (экстрактор на bench-сцене), существующие проверки пакетов и набора 2 переведены на кеш.

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
- **R7 разбит на три PR** (решение владельца): каждый — draft в `aibranch`, `refs #118`.
  - **R7a** — пулы в бэкендах и typed handle за текущим API: `ds::handle_pool<T, Handle>` (`snk/common/ds/ds_handle_pool.hpp`, слот-мап со свободным списком и generation; `slot_table` теперь его алиас), `texture_handle = handle<texture_tag>` в `rnd_handle.h`, `texture_pool` (`rnd_texture_pool.h`) в `gl::driver`, `null::driver` и validation. Handle получают `render_pass_desc`/`attachment_desc`, `binding_res`, `read_texture` и новые `driver_interface::info(texture_handle)` / `is_alive(texture_handle)`; `set_target_formats` стала `set_target_formats(drv, desc, pass)`. Владение не менялось: `create_texture` по-прежнему отдаёт `unique_ptr<texture_interface>`, handle выдаётся при создании и протухает (generation растёт) при уничтожении объекта. Validation: правило `stale_handle` (handle уничтоженной текстуры, в том числе слот, занятый новой) и `dead_object` для handle, которого драйвер не выдавал. `sampler_handle` и `destroy_texture(handle)` с отложенным освобождением придут вместе с переносом владения в R7b.
  - **R7b1** (владение у драйвера): `create_texture` отдаёт `texture_handle`, `destroy(texture_handle)` (в GL и null освобождение немедленное, handle протухает сразу), текстуры живут в `texture_pool` драйвера (он владеет ими, без `std::function` и `shared_ptr` на текстуру) и освобождаются вместе с драйвером. Старший байт `gen` handle'а — id драйвера (8 бит, ids 1..255 по кругу), младшие 24 бита — generation: чужой handle (другого драйвера или внутренний handle обёрнутой validation текстуры) не проходит проверку generation и считается неизвестным (`dead_object`), а не протухшим. `handle<>` по-прежнему 8 байт. Временный мост до R7c: `driver_interface::resolve(handle) -> texture_interface*`; RAII-владелец `texture_owner` (`rnd_texture_owner.h`) держит текстуру в `texture_manager` и `material_groups`.
  - **R7b2** (`texture_id` в `texture_manager` и его потребителях): `rnd_texture_id.h` — `texture_id` (индекс слота, 0 = нет), `texture_binding {handle, generation}` и `texture_resolver`. `texture_manager` держит слот на тег (`require(tag) -> texture_id`, `generate`, `id_of`, `binding(id)`, `is_ready`); слот переживает загрузку, перезагрузку, resize и `remove`, его generation растёт при каждой смене объекта за id. `runtime_data::samplers` и `compiled_material` держат `texture_id`; `material_groups(drv, texture_resolver)` ключует bind group по `(id, generation слота)`, плейсхолдер для слота без объекта; `render_context::texture(id)` для скайбокса. Мосты до R7b3/R7c: `find`/`generate_texture` (возвращают `texture_interface*`) остаются для `render_targets`, `capture_service` и редактора.
  - **R7b3** (render targets и захват): `render_targets::registry` (`rnd_render_targets.h`) — `target_id {slot, gen}` на тег, `ensure/find/resolve/is_current/current`; слот переживает пересоздание цели, её `gen` растёт, устаревший id не резолвится (вместо висячего указателя). `capture_service` читает цель по `texture_handle` через `read_texture`.
  - **R7c** (ImGui по handle, мосты убраны, строгий гейт; закрывает критерий #119 «вне `drv_*` нет GL»):
    - `gui::imgui_backend_interface::to_imgui(texture_handle) -> ImTextureID` вместо `get_imgui_texture_from_texture(texture_interface*)`. Базовый класс получает `driver_interface&` в конструкторе (`gui_init` берёт его у `render_system`) и спрашивает у драйвера новый `native_texture_name(handle)`: имя GL у `gl::driver`, имя обёрнутого драйвера у validation, серийный номер текстуры у null, 0 для null/протухшего/чужого handle. Каст к `gl::texture` и включение `rnd_gl_texture.h` в `gui/backends/opengl` ушли; stock `imgui_impl_opengl3` остался. Отличие от схемы выше: вместо внутреннего `gl::driver::native_texture` с PRIVATE-доступом — виртуальный `native_texture_name` в `driver_interface`, потому что драйвер приходит как `driver_interface&` (в Debug он может быть validation над GL), а не как `gl::driver`. Панели (viewport, game view, prefab) берут handle из `texture_manager::binding(id_of(tag)).handle` и отдают его в `to_imgui`.
    - Убраны мосты: `driver_interface::resolve`, `texture_owner::get()/operator->/operator*`, `texture_manager::find` и `generate_texture` (теперь `generate(tag, size, format, data)` отдаёт `texture_id`), `render_context::find_texture/ensure_render_target` отдают `texture_handle`, `render_targets::ensure` отдаёт `texture_handle` (размер и формат существующей цели читаются через новый `texture_manager::info(texture_id)`, то есть `driver::info(handle)`). Загрузчик кубмапы пишет грани через новый `driver_interface::update_texture(handle, data, texture_region)` (то же, что `texture_interface::update_data`).
    - Объект за handle остался только внутри бэкендов: `texture_object_access` (`rnd_texture_backend.h`) реализуют gl, null и validation рядом с `driver_interface`; им пользуются validation (оборачивает объект обёрнутого драйвера) и тесты драйверов. Рендерер, редактор и GUI его не видят. `validation::driver::inner_handle(handle)` стал публичным (тесты сверяют handle обёртки и обёрнутого драйвера).
    - `texture_manager::generation()` **остаётся**: это не только оптимизация. `require(tag)` запускает загрузку, а вызывает его только компиляция материала; слот, опустошённый `remove`/`clear_cache`, заново запрашивается тем, что материал пересобирается по этому счётчику (тест `ReloadingATextureBuildsTheGroupsAgainWithTheNewTexture`). Сегодня `remove` зовут редиректор камер редактора и `render_targets::ensure` (цели рендера, у них нет загрузчика), `clear_cache` не зовёт production-код; счётчик нужен для перезагрузки по `remove`, которую ещё предстоит подключить. Bind group от него не зависят (ключ `(id, generation слота)`). Убрать счётчик можно, когда перезагрузку запросит сам `texture_manager` (помеченный слот запрашивается заново в `begin_frame`).
    - `texture_manager` помнит поток, в котором создан, `ASSERT_MSG(on_owner_thread())` в `require/generate/binding/remove/clear_cache`: он не берёт локов, а другие потоки (MCP `viewport.capture`) уходят в главный через `main_thread_queue`. `texture_resolver` стал `SNK_API` (его наследник `texture_manager` экспортируется, иначе MSVC C4275).
    - Гейт `tools/ci/check_no_gl_outside_drivers.sh` строгий в обе стороны, а исключение задано путём и допустимой строкой (regex): другой GL в тех же файлах падает. Второе правило пускает `texture_object_access`/`rnd_texture_backend.h`/`texture_object(` только в `core/core/video/drivers/` и в тесты `rnd_null_driver_tests.cpp`, `rnd_validation_tests.cpp`. Итоговый allow-list: (1) `core/core/windows/window_service/impl_glfw/` — оконный бэкенд GLFW создаёт контекст, делает его текущим, свопит буферы; разрешён только `#include <GLFW/glfw3.h>`; (2) `core/engine/render/render_system/rnd_render_service_init.cpp` — выбор бэкенда (`render/backend`), разрешены только `rnd_gl_swapchain.h` и `gl::swapchain`; (3) `core/engine/gui/backends/opengl/` — разрешён только stock `imgui_impl_opengl3` / `ImGui_ImplOpenGL3_*` (по решению владельца остаётся; цель не линкуется с `drv_opengl`, loader у imgui встроенный). Всё остальное вне `core/core/video/drivers/impls/opengl/` с GL-API падает в CI; запись списка без попаданий тоже.
    - Что остаётся: записи allow-list выше — не долг R7, а границы (контекст окна, выбор бэкенда, stock-бэкенд ImGui). Следующие шаги RHI не требуют GL вне драйвера.

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
   - **Сделано в R1** (PR #251): `constant_data::variant_hash` (FNV-1a, не зависит от порядка и платформы) и `shader_config_cache` в `ShaderManager`; `draw_call_t` = `variant_id` + указатель на `compiled_material` (компонент на entity, `material_desc_component::dirty`); `OPAQUE` — pass-define варианта; `z_prepass` берёт вариант с подменой программы из кеша. Кеш материала пересобирается при смене desc, флага скиннинга или поколения `texture_manager` (hot reload, ресайз RT). US-45-2 (pmr-контейнеры) не делался: кешированные конфиги живут дольше кадра, кадровый аллокатор для них запрещён.
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
   - **Baseline R0** (GCC Release, ubuntu-latest, null-бэкенд, мс за кадр; запуск CI 39175304070, коммит edfb68a; `ExtractRenderData` — 1 вызов за кадр, каждая зона `Extract.*` — 200 вызовов: 2 камеры × 100 мешей):

     | Зона | mean | p50 | p95 | max |
     |---|---:|---:|---:|---:|
     | `ExtractRenderData` | 0.250 | 0.246 | 0.263 | 0.292 |
     | `Extract.ShaderDesc` | 0.132 | 0.130 | 0.145 | 0.164 |
     | `Extract.PushDraws` | 0.040 | 0.039 | 0.041 | 0.056 |
     | `Extract.BuildDC` | 0.010 | 0.010 | 0.010 | 0.022 |
     | `Extract.Skinning` | 0.010 | 0.009 | 0.010 | 0.022 |

     Разброс между запусками на общих раннерах около ±20% (первый запуск того же кода дал `ExtractRenderData` mean 0.201): сравнивать порядок величин, не проценты.

     Критерий EPIC-45 (< 2 мс на 100 мешей) на этом baseline выполнен с большим запасом; цели `Extract.ShaderDesc`/`Extract.PushDraws` из п. 1 (< 0.5 мс на 79 мешей) тоже.

   - **После R1** (GCC Release, ubuntu-latest, null-бэкенд, мс за кадр; запуск CI 39179644383 PR #251, коммит 290207c; те же условия и сцена):

     | Зона | mean | p50 | p95 | max | baseline mean |
     |---|---:|---:|---:|---:|---:|
     | `ExtractRenderData` | 0.083 | 0.081 | 0.094 | 0.097 | 0.250 |
     | `Extract.ShaderDesc` | 0.010 | 0.010 | 0.010 | 0.023 | 0.132 |
     | `Extract.PushDraws` | 0.015 | 0.014 | 0.015 | 0.026 | 0.040 |
     | `Extract.BuildDC` | 0.009 | 0.009 | 0.009 | 0.024 | 0.010 |
     | `Extract.Skinning` | 0.008 | 0.008 | 0.008 | 0.020 | 0.010 |

     Сцена не меняется между кадрами, поэтому `Extract.ShaderDesc` — это проверка валидности кеша (< 0.2 мс из US-45-3 с запасом). Разброс раннеров прежний (±20%), но выигрыш в разы выходит за него.

   - **После R6** (GCC Release, ubuntu-latest, null-бэкенд, мс за кадр; push-прогоны `aibranch`: до R6b — запуск 39301753351, коммит c2c86c7 (R6a); после — запуск 39316196481, коммит 8a4595e (R6b); те же условия и сцена):

     | Зона | mean | p50 | p95 | max | mean после R6a | baseline mean |
     |---|---:|---:|---:|---:|---:|---:|
     | `ExtractRenderData` | 0.083 | 0.081 | 0.094 | 0.106 | 0.090 | 0.250 |
     | `Extract.ShaderDesc` | 0.008 | 0.008 | 0.009 | 0.022 | 0.010 | 0.132 |
     | `Extract.PushDraws` | 0.008 | 0.008 | 0.009 | 0.011 | 0.009 | 0.040 |
     | `Extract.BuildDC` | 0.013 | 0.012 | 0.014 | 0.033 | 0.013 | 0.010 |
     | `Extract.Skinning` | 0.009 | 0.009 | 0.009 | 0.020 | 0.009 | 0.010 |
     | `Extract.SortDraws` | 0.005 | 0.005 | 0.005 | 0.007 | 0.005 | — |

     POD-пакет (R6a) и кеш инстансов (R6b) не изменили время extract в пределах разброса раннеров: компиляция при изменении уже была в R1, а R6 убрал тяжёлые поля из `draw_call_t` и таблицу материалов из пакета. Критерий EPIC-45 (< 2 мс на 100 мешей) выполнен с запасом ~25×.

## 5. План по PR (после PR-5 #109 и #174; волны — `STAGE_D_PLAN.md` §2)

| PR | Issue | Содержимое | Пассы |
|---|---|---|---|
| R0 | #119 (часть) | `drv_null`, `validation_driver`, `render/backend`, тесты, baseline-бенчмарк, CI-grep «GL вне `drv_*`» с allow-list (`impl_glfw` hints), эталонные PNG | — |
| R1 | #127 | variant key, кеш конфигов и материалов, pass-define `OPAQUE` | все (только `configure_pass`) |
| R2 | #115a | usage-флаги, JSON-совместимость, `render_targets::ensure` | все RT-пассы |
| R3 | #115b | `begin/end_render_pass`, кеш FBO; удаление push/pop/clear/scissor | z_prepass+opaque → skybox+debug → transparent+composition |
| R4 | #116 | `pipeline_desc`/кеш, трим топологий, debug_pass через кеш; удаление `set_render_state` | все |
| R5a | #117 | `SNK_*`, bind groups, наборы 0, 1, 3, ring UBO, кадровый SSBO костей, lights count | все |
| R5b | #117 (+#121, #212) | `MaterialParams` и набор 2; удаление `uniform()` | все, затем GUI |
| R6 | #124, #125 | POD `draw_call_t`, `material_instance`, повторный замер | extract + все |
| R7 | #118 | handle pools, `texture_id`, `to_imgui`, generation в validation; закрытие #119 (критерий «вне `drv_*` нет GL») | все + editor |

- **Неизменность рендера** на каждом PR:
  - (1) `editor-smoke` (`build.yml:311-377`, `check_smoke.sh`) без изменений;
  - (2) сравнение с эталонами `smoke`/`game_view`/`checklist` (llvmpipe) в `check_smoke.sh`, блокирующее с 2026-10-19 (fuzz 4%, порог 2%);
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
