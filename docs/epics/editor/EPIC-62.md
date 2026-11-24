# EPIC-62: Импорт ассетов с настройками (`.resource`: importer + settings)

**Theme:** editor
**Status:** in_progress (#1193 — импорт PNG/JPEG; дальше #1194, #1195, #1371)
**Issue:** #34
**Stories:** #1193, #1194, #1195, #1371

---

## 1. Зачем

Сейчас картинка попадает в проект только как побочный продукт
импорта модели или копированием руками, а параметры текстуры
(wrap, фильтр, мипы) правятся в `*.txm.desc` вручную. Импорт с настройками
записывает «рецепт» импорта рядом с исходником, чтобы повторный
импорт (#1194) дал тот же результат с тем же ID.

## 2. `.resource`: importer и settings

- Владелец настроек — `.resource` **исходника** (`rock.png.resource`), а не выход
  импорта. Формат `.resource` не меняется (EPIC-57, 4.2): `importer` — строка,
  `settings` — объект.
- Движок `importer`/`settings` не читает никогда: это рецепт редактора.
  Движок читает только выход (`rock.png.txm.desc`).
- `.resource` пишется первым, новым файлом (никогда поверх), в
  каноническом виде `res::write_asset_meta`; база ассетов, увидев картинку,
  берёт его guid и своего не создаёт.

## 3. `texture`, версия 1 (#1193)

Импорт `<abs>/rock.png` в папку `F` (png, jpg, jpeg) пишет в верхний слой проекта,
по порядку:

1. `res://F/rock.png.resource` — новый guid, `importer: "texture"`, `settings`, `flags: []`;
2. `res://F/rock.png` — байты исходника без перекодирования;
3. `res://F/rock.png.txm.desc` — `texture_2d_desc`: `__parent` = `res://base_texture.desc`,
   `name`, `data` = `res://F/rock.png`, `header`.

Каждая запись — создание нового файла без следования по
ссылкам; при сбое уже записанное удаляется в обратном порядке.

| Ключ | Тип | По умолчанию | Значения |
|---|---|---|---|
| `version` | int | 1 | только 1 |
| `wrap` | string | `clamp_to_edge` | `clamp_to_edge`, `repeat`, `repeat_mirror`, `clamp_to_border` |
| `filter` | string | `nearest` | `nearest`, `linear` |
| `mips` | bool | `false` | |

Разбор строгий: неизвестный ключ, неверный тип или значение —
отказ с именем ключа; отсутствующий ключ — значение по
умолчанию (как в `base_texture.desc`).

Отображение в `header`:

- `wrap` = `wrap`; `mag` = `filter`;
- `mips: false` → `min` = `filter`, `data.mip_levels` = 1;
- `mips: true` → `min` = `linear_mipmap_nearest` (единственная строка с мипами в
  словаре `texture_desc`, загрузчик даёт GL_LINEAR_MIPMAP_LINEAR; так же и при
  `filter: nearest`), `data.mip_levels` = 32 (загрузчик ограничивает
  полной цепочкой и строит уровни);
- `type`, `usage`, `extent`, `format` не пишутся: загрузчик берёт их из картинки
  (#926).

Кто вызывает: команда `file.import_image {path, folder?, name?, settings?}` (агент —
только с подтверждением человека, `risk = confirm`, задача `{job}`) и пункт
«Import Image...» в контекстном меню папки Asset Browser (выбор файла, затем
окно с wrap / filter / mips и итоговыми именами).

## 4. `model` (#1195)

Заготовка: настройки импорта модели будут описаны здесь.

## 5. Повторный импорт (#1194)

Настройки читаются из `.resource` исходника; выходы пересоздаются по
тем же путям; guid и settings сохраняются.

## 6. Вне эпика

sRGB и сжатие текстур — #1371.
