# EPIC-14: Component UI Registry — type-safe editor

**Theme:** prefab-first
**Status:** done
**Depends on:** EPIC-08, EPIC-10

## Цель

Редактор не должен знать строковые имена типов десков в нескольких местах.
Сейчас строки вида `"camera_desc"`, `"directional_light_desc"` раскиданы по трём местам:
1. `edt_cr_camera.cpp` — `panel.add_desc_renderer("camera_desc", ...)`
2. `edt_inspector_panel.cpp` — `{"Camera", "camera_desc"}` в `draw_add_component_popup`
3. `edt_component_renderers.h/.cpp` — косвенно через `add_desc_renderer`

Если добавить новый компонент — надо не забыть обновить ВСЕ три места.

**Решение:** единый `component_ui_registry`. Каждый тип регистрируется ровно в одном месте —
в соответствующем `edt_cr_*.cpp`. Инспектор работает только с реестром, не знает типов.

**Второй принцип:** на сайтах регистрации использовать `T::__type` константу (compile-time),
а не строковый литерал. Переименование типа становится ошибкой компилятора, а не тихим багом.

---

## Архитектурный дизайн

### `edt::component_ui_registry`

```cpp
// edt_component_ui_registry.h
namespace edt {

struct component_ui_entry {
    std::string      display_name;
    boost::json::object default_overrides; // JSON при создании нового компонента
    // nullptr → generic JSON renderer
    std::function<bool(scn::prefab_desc::prefab_comp_node&)> renderer;
};

class component_ui_registry {
    // std::map для стабильного порядка в UI
    std::map<std::string, component_ui_entry> m_entries;
public:
    void register_component(std::string_view type_name, component_ui_entry entry);
    const component_ui_entry* find(std::string_view type_name) const;
    void foreach(std::function<void(const std::string&, const component_ui_entry&)>) const;
};

} // namespace edt
```

### Регистрация в `edt_cr_camera.cpp`

```cpp
void edt::edt_cr_camera_register(edt::component_ui_registry& registry)
{
    registry.register_component(
        scn::camera_desc::__type,   // ← compile-time constant, не строковый литерал
        {
            "Camera",
            {},
            [](scn::prefab_desc::prefab_comp_node& comp) -> bool { ... }
        }
    );
}
```

### Изменение `inspector_panel`

- Убрать `m_desc_renderers` (`unordered_map<string, desc_comp_renderer>`)
- Убрать `add_desc_renderer()`
- Добавить `set_component_ui_registry(component_ui_registry*)`
- `draw_add_component_popup` — итерирует реестр вместо hardcoded таблицы
- Рендер компонента — берёт `renderer` из реестра или fallback на `render_generic_json`

### Сигнатуры `edt_cr_*_register` меняются

```cpp
// Было:  void edt_cr_camera_register(inspector_panel&)
// Стало: void edt_cr_camera_register(component_ui_registry&)
```

`edt_component_renderers.cpp`:
```cpp
void edt::register_desc_component_renderers(edt::component_ui_registry& registry) {
    edt::edt_cr_camera_register(registry);
    edt::edt_cr_light_register(registry);
    edt::edt_cr_skybox_register(registry);
    edt::edt_cr_skin_register(registry);
}
```

---

## US-14-1: Добавить `static constexpr __type` к старым десках

**Файлы:** `scn_camera_desc.h`, `scn_directional_light_desc.h`, `scn_skybox_desc.h`,
`scn_material_desc.h`, `scn_object_desc.h` (если нет)

Новые дески (`bone_desc`, `skinning_desc`, `mesh_node_desc`, `animations_desc`) уже имеют
`static constexpr std::string_view __type`. Старые — нет.

```cpp
class camera_desc : public desc::desc_base {
public:
    static constexpr std::string_view __type = "camera_desc";
    ...
};
```

> Значение `__type` должно совпадать со строкой, передаваемой в `desc_system.register_type<T>(...)`.

**AC:**
- Все компонентные дески (camera, light, skybox, material, object) имеют `static constexpr __type`
- Строка в `register_type` совпадает с `::__type`

---

## US-14-2: Создать `edt::component_ui_registry`

**Файлы:** `Editor/code/editor_system/edt_component_ui_registry.h` (новый)
           `Editor/code/editor_system/edt_component_ui_registry.cpp` (новый)

Реализовать класс по архитектурному дизайну выше.

**AC:**
- `register_component` добавляет запись
- `find` возвращает `nullptr` если тип не зарегистрирован
- `foreach` вызывает fn для каждой записи в алфавитном порядке (std::map)

---

## US-14-3: Перевести `edt_cr_*.cpp` на `component_ui_registry`

**Файлы:** `edt_cr_camera.cpp`, `edt_cr_light.cpp`, `edt_cr_skybox.cpp`, `edt_cr_skin.cpp`

Изменить сигнатуры:
```cpp
// Было:
void edt::edt_cr_camera_register(edt::inspector_panel& panel)
// Стало:
void edt::edt_cr_camera_register(edt::component_ui_registry& registry)
```

Использовать `T::__type` вместо строковых литералов:
```cpp
registry.register_component(scn::camera_desc::__type, { "Camera", {}, renderer });
registry.register_component(scn::directional_light_desc::__type, { "Directional Light", {}, renderer });
```

Обновить `edt_cr_internal.h` — изменить forward declarations и сигнатуры.
Обновить `edt_component_renderers.h/.cpp` — `register_desc_component_renderers(component_ui_registry&)`.

**AC:**
- Ни одного строкового литерала типа `"camera_desc"` в `edt_cr_*.cpp`
- Все используют `T::__type`

---

## US-14-4: Рефакторинг `inspector_panel`

**Файлы:** `edt_inspector_panel.h`, `edt_inspector_panel.cpp`

1. Убрать `m_desc_renderers`, метод `add_desc_renderer`
2. Добавить `component_ui_registry* m_ui_registry = nullptr`
3. Добавить `void set_component_ui_registry(component_ui_registry* r)`
4. `draw_add_component_popup` — итерация реестра:
```cpp
m_ui_registry->foreach([&](const std::string& type, const component_ui_entry& e) {
    if (!m_selected_node->components.count(type)) {
        if (ImGui::MenuItem(e.display_name.c_str())) {
            scn::prefab_desc::prefab_comp_node comp;
            comp.type_name = type;
            comp.overrides = e.default_overrides;
            m_selected_node->components[type] = std::move(comp);
            changed = true;
        }
    }
});
```
5. Рендер компонента — lookup через реестр:
```cpp
const auto* ui = m_ui_registry ? m_ui_registry->find(comp.type_name) : nullptr;
if (ui && ui->renderer)
    changed |= ui->renderer(comp);
else
    render_generic_json(comp.overrides);
```

**AC:**
- `inspector_panel` не содержит ни одного строкового имени типа
- "Add Component" popup работает без hardcoded таблицы

---

## US-14-5: Подключение реестра в `edt_editor_system`

**Файлы:** `edt_editor_system.h`, `edt_editor_system.cpp`

1. Добавить `edt::component_ui_registry m_component_ui_registry` в `editor_system`
2. В `init()`:
```cpp
edt::register_desc_component_renderers(m_component_ui_registry);
m_inspector_panel->set_component_ui_registry(&m_component_ui_registry);
```
3. Убрать старый вызов `register_desc_component_renderers(*m_inspector_panel)`.

**AC:**
- Реестр создаётся один раз в `editor_system`, передаётся в инспектор
- Инспектор корректно показывает все зарегистрированные типы

---

## Критерии готовности

- [ ] `component_ui_registry` создан и работает
- [ ] `inspector_panel` не содержит строковых имён типов
- [ ] `draw_add_component_popup` итерирует реестр
- [ ] Все сайты регистрации используют `T::__type` вместо строковых литералов
- [ ] Все компонентные дески имеют `static constexpr __type`
- [ ] Регистрация нового типа требует изменений только в одном `edt_cr_*.cpp`
