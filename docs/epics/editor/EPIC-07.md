# EPIC-07: Material & Shader Editor

**Theme:** editor
**Status:** in progress
**Issue:** [#35](https://github.com/Prorok1015/OpenGL_Engine/issues/35) · истории: [#1196](https://github.com/Prorok1015/OpenGL_Engine/issues/1196) (US-07-A), [#1197](https://github.com/Prorok1015/OpenGL_Engine/issues/1197) (US-07-B), [#1198](https://github.com/Prorok1015/OpenGL_Engine/issues/1198) (US-07-C), [#1370](https://github.com/Prorok1015/OpenGL_Engine/issues/1370)
**Depends on:** EPIC-10 (Inspector с desc-рендерерами), EPIC-08
**Версия документа:** v2 — истории переписаны под существующие issues; v1 (US-07-1..5) описывал рендерер материала в Inspector

## Цель

Окно редактирования материала: поля `material_desc` (схема v2), define'ы шейдера,
живое превью несохранённых правок и ошибки компиляции шейдера — прямо в редакторе.
Текстовый редактор шейдеров — отдельная работа (#1202, маркеры проблем #1391).

## Старые истории → новые

| v1 | Где теперь |
|---|---|
| US-07-1 Material Inspector renderer | US-07-A (окно материала вместо рендерера в Inspector), виджеты текстур/сэмплеров — US-07-B |
| US-07-2 Material Browser | двойной клик по материалу в Asset Browser открывает окно (US-07-A); миниатюры-превью не сделаны, остаются планом |
| US-07-3 Material desc hot-reload | US-07-A: Save → `store_sync` + `signal_changed`; несохранённые правки видны только в превью |
| US-07-4 Shader editor (text) | #1202 (документ исходника шейдера), #1391 (маркеры проблем) |
| US-07-5 Shader define toggles | US-07-A, раздел Defines |
| — | US-07-C: создание `material_desc` (#1198) |
| — | #1370: просмотр материалов движка/пакетов только для чтения |

---

## US-07-A: Окно материала (#1196)

Материал — data document (`is_document`): окно выбирается по `__type` открытого файла,
есть undo/redo, Save, «Open as data» в контекстном меню. Материалы движка и пакетов
сейчас не открываются (отказ с причиной); просмотр только для чтения — #1370.

### Поля (схема v2)

| Поле | Ключ в файле | Виджет |
|---|---|---|
| `shader` | `/shader` | ссылка на `.shader.desc` (`shader_desc`); inline запрещён (#1200) |
| `albedo` | `/uniforms/albedo` | ColorEdit4 |
| `emissive` | `/uniforms/emissive` | ColorEdit4 |
| `shininess` | `/uniforms/shininess` | DragFloat (0..1000) |
| `queue` | `/queue` | opaque / transparent / mix, null — ни в одной |
| `render_mode` | `/render_mode` | Combo |
| `samplers_textures` | `/samplers` | позиционный список, null — пустой слот; виджеты — US-07-B |
| `defines` | `/defines` | раздел Defines (ниже) |

Поля `shader_vertex`/`shader_fragment`/`specular`/`ambient` схемы v1 удалены.

### Defines

Строки — имена из `#ifdef`/`#ifndef`/`defined(...)` stage-файлов шейдера (с include'ами),
свои и унаследованные define'ы материала и введённые вручную. `USE_ANIMATION` и `OPAQUE`
скрыты: их ставит компилятор материала. Состояния: свой / унаследованный / выключен;
запись идёт в `/defines`, не в `overrides`; «Reset to inherited» убирает свой список.

### Превью

Общий `preview_stage`: форма сфера/куб/плоскость, «Sky» или нейтральный фон, орбитальная
камера, парковка скрытого окна. Материал рисуется из `memory://`-копии `shown()`, поэтому
несохранённые правки видны сразу. У sky-материала превью нет.

### Ошибки компиляции

Раздел «Compile errors» под Defines читает снимок последнего рендера
(`render_system::shader_problems()`, источник Problems `shader`), без нового API движка:
- «This material»: строки `shader/load` самого документа и его превью, строки файлов шейдера,
  чей набор define'ов (`shader_desc::config(defines)`, ± `OPAQUE`) совпадает с набором
  материала; вариант с `USE_ANIMATION` помечен «skinned», он есть только если уровень рисовал
  skinned-меш;
- «Other variants of this shader» (свёрнуто): остальные строки файлов шейдера с их набором;
- проверка препроцессора (`shader_config::load`, без GPU) через ~300 мс после правки define'ов,
  строки помечены «preprocess»;
- предупреждения своих define'ов: не идентификатор, больше одного токена значения, имя движка
  (`MAX_LIGHT_COUNT`, stage define), компилятора, define шейдера или его исходника;
- у sky-материала — строки последнего кадра уровня с пометкой;
- Go to открывает файл строки (`open_shader_problem_file`).

Формат имени программы (`{ A | B }`) и его разбор — одни inline-функции в
`rnd_scene_shader_desc.h`. Ограничение: ошибка внутри вставленного define'а пока относится к
строке stage-файла (#1387).

---

## US-07-B: Виджеты текстур и сэмплеров (#1197)

Слоты `/samplers` по схеме шейдера: выбор текстуры, пустой слот, превью-миниатюра.

---

## US-07-C: Создание material desc (#1198)

Создание нового `material_desc` из редактора: шейдер, родитель, место в проекте.

---

## #1370: Материалы движка и пакетов только для чтения

Окно открывает такие материалы для просмотра (поля, define'ы, превью, ошибки) без Save.

---

## Критерии готовности

- [x] Окно материала — data document: поля схемы v2, undo/redo, Save (US-07-A)
- [x] Раздел Defines: свои, унаследованные, из шейдера; запись в `/defines` (US-07-A)
- [x] Живое превью несохранённых правок, формы и фон (US-07-A)
- [x] Ошибки компиляции текущего набора define'ов с Go to, проверка препроцессора и define'ов (US-07-A)
- [ ] Виджеты текстур и сэмплеров (US-07-B, #1197)
- [ ] Создание material desc (US-07-C, #1198)
- [ ] Материалы движка/пакетов только для чтения (#1370)
- [ ] Текстовый редактор шейдеров с маркерами проблем (#1202, #1391)
