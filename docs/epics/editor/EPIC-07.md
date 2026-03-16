# EPIC-07: Material & Shader Editor

**Theme:** editor
**Status:** planned
**Depends on:** EPIC-10 (Inspector с desc-рендерерами), EPIC-08

## Цель

Панель редактирования материалов и шейдеров: визуальный просмотр,
редактирование параметров `material_desc` прямо в редакторе,
live-preview изменений через hot-reload.

---

## US-07-1: Material Inspector renderer

**Файлы:** `edt_component_renderers.hpp` (или отдельный файл после EPIC-06)

Расширить рендерер для `material_desc` компонента в Inspector:

| Поле | Виджет |
|---|---|
| `albedo` | ColorEdit4 |
| `specular` | ColorEdit4 |
| `ambient` | ColorEdit4 |
| `emissive` | ColorEdit4 |
| `shininess` | DragFloat (0..1000) |
| `queue` | Combo (opaque / transparent / mix) |
| `samplers_textures[]` | список tag-строк + кнопка "+" / "-" |
| `shader_vertex` | InputText + кнопка browse |
| `shader_fragment` | InputText + кнопка browse |
| `defines[]` | список строк + toggle |

При изменении любого поля → обновляем `prefab_comp_node.overrides` → `on_node_changed`.

---

## US-07-2: Material Browser

Вкладка в Asset Browser (или отдельная панель): список всех `.desc` файлов с
`"__type": "material_desc"`. Превью — цветной квадрат с albedo цветом (или
thumbnail сферы если есть render-to-texture).

Двойной клик на материале — открывает его в Material Inspector.

---

## US-07-3: Material desc hot-reload

Правка полей материала в Inspector → `signal_changed(material_tag)` →
hot-reload пересоздаёт материал → рендер обновляется в реальном времени.

Сейчас hot-reload для материалов работает через `component_tracker` (если
material был загружен через `__parent`). Нужно убедиться что путь работает
для inline-материалов тоже.

---

## US-07-4: Shader editor (text)

Простой текстовый редактор GLSL в отдельной панели:
- Открыть через двойной клик на `.vert` / `.frag` в Asset Browser
- Многострочный `InputTextMultiline` с базовой подсветкой (ImGui не поддерживает
  из коробки — использовать ImGuiColorTextEdit если добавить в lib3dparty)
- Ctrl+S → сохранить на диск → `signal_changed(shader_tag)` →
  шейдер перекомпилируется (через существующий `rnd_shader_manager`)

---

## US-07-5: Shader define toggles в материале

В Inspector материала — список `defines` как checkbox'ы.
`USE_ANIMATION`, `USE_NORMAL_MAP`, `OPAQUE` и т.д. визуально переключаются.
Изменение → `overrides["defines"]` обновляется → hot-reload.

---

## Критерии готовности

- [ ] Inspector отображает все поля `material_desc` с нужными виджетами
- [ ] Изменение albedo/specular/shininess сразу видно в вьюпорте (hot-reload)
- [ ] Asset Browser показывает `.desc` материалы
- [ ] Шейдер `.vert/.frag` открывается и сохраняется из редактора
- [ ] Сохранение шейдера → автоматическая перекомпиляция
