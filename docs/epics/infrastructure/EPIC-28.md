# EPIC-28: Resource Cache — кеш по тегу + типу ресурса

**Theme:** infrastructure
**Status:** planned
**Depends on:** —

---

## Цель

Изменить ключ кеша в `resource_system` с `res::tag` на `{res::tag, type_id}`, чтобы один и тот же файл мог быть загружен через разные адаптеры без конфликтов.

---

## Проблема

Текущий кеш в `res_system.h`:

```cpp
std::unordered_map<res::tag, cache_entry> m_cache;
```

Ключ — только `res::tag`. Если один файл запрашивается через два разных типа:

```cpp
auto text = m_res.require<res::text_resource>(tag);   // Адаптер: text_adapter
auto desc = m_res.require<desc::desc_base>(tag);       // Адаптер: desc_adapter
```

Второй вызов вернёт закешированный `text_resource` из первого — это wrong type, crash или silent corruption при `polymorphic_pointer_cast`.

**Где это реально происходит:**
- `load_desc_template()` загружает `.desc` как `text_resource` для ручного парсинга
- `level_manager::load()` загружает `.desc` как `level_desc` через desc pipeline
- Модельный импорт может загружать одни и те же файлы как raw bytes и как desc
- Editor может открыть файл как текст (для просмотра) и как desc (для редактирования)

---

## Решение

### Составной ключ кеша

```cpp
struct cache_key {
    res::tag tag;
    ds::type_id adapter_type;  // тип ресурса (text_resource, desc_base, picture_resource, ...)

    bool operator==(const cache_key&) const = default;
};

struct cache_key_hash {
    std::size_t operator()(const cache_key& k) const {
        auto h1 = std::hash<res::tag>{}(k.tag);
        auto h2 = std::hash<ds::type_id>{}(k.adapter_type);
        return h1 ^ (h2 << 1);
    }
};

// Было:
std::unordered_map<res::tag, cache_entry> m_cache;

// Стало:
std::unordered_map<cache_key, cache_entry, cache_key_hash> m_cache;
```

### Изменения в require_resource_internal

```cpp
template<class RESOURCE, bool is_sync>
auto require_resource_internal(const res::tag& tag) {
    cache_key key{ tag, ds::type_id::make<RESOURCE>() };

    if (auto cached_res = try_get_cached_resource(key)) {
        return res_handle<RESOURCE>(cached_res, tag);
    }
    // ...
}
```

### Затронутые методы

| Метод | Изменение |
|---|---|
| `try_get_cached_resource` | Принимает `cache_key` вместо `tag` |
| `push_resource_to_cache` | Принимает `cache_key` вместо `tag` |
| `push_resource_to_strong_cache` | Аналогично |
| `require_resource_internal` | Строит `cache_key` из `tag` + `type_id` |
| `signal_changed` | Evict все записи с данным `tag` (любой тип) |
| `exists` | Проверяет resolver, не кеш (без изменений) |
| `store` | Пишет в resolver, не в кеш (без изменений) |

### signal_changed — evict по тегу

При `signal_changed(tag)` нужно удалить **все** cache entries с данным тегом (независимо от типа):

```cpp
void signal_changed(const res::tag& tag) {
    {
        std::unique_lock lock(m_cache_mutex);
        // Удалить все записи где key.tag == tag
        std::erase_if(m_cache, [&](const auto& pair) {
            return pair.first.tag == tag;
        });
    }
    notify_watchers(tag);
}
```

---

## User Stories

### US-28-1 — Составной ключ кеша

**Файлы:**
- `core/core/resource/res_system.h` — `cache_key`, все методы кеша
- `core/core/resource/res_system.cpp` — `signal_changed`, `register_alias`

**Изменения:** заменить `res::tag` на `cache_key` в `m_cache` и `m_pinned_resources`.

### US-28-2 — Обновить signal_changed и watchers

**Файлы:**
- `core/core/resource/res_system.h` — `signal_changed`

**Изменения:** evict все типы при `signal_changed(tag)`. Watchers остаются по `res::tag` (они не зависят от типа).

### US-28-3 — Обновить register_alias

**Файлы:**
- `core/core/resource/res_system.cpp`

**Изменения:** alias работает на уровне `res::tag` (до построения `cache_key`), без изменений в семантике.

---

## Файлы

| Файл | Изменение |
|---|---|
| `core/core/resource/res_system.h` | `cache_key`, все cache методы, `m_cache`, `m_pinned_resources` |
| `core/core/resource/res_system.cpp` | `signal_changed`, `register_alias` |

---

## Риски

| Проблема | Решение |
|---|---|
| Больше памяти (дубли для одного файла) | На практике один файл редко загружается двумя типами; raw bytes разделяются через resolver |
| `signal_changed` должен evict все типы | `std::erase_if` по `key.tag == tag` |
| Наследование типов (`desc_base` vs `level_desc`) | `type_id` берётся от запрашиваемого типа `T`, не от результата адаптера; `find_adapter_recursive` уже обрабатывает наследование |
| Обратная совместимость | Внешний API (`require<T>(tag)`) не меняется |

---

## Критерии готовности

- [ ] Один файл может быть загружен как `text_resource` и как `desc_base` одновременно без конфликта
- [ ] `signal_changed(tag)` инвалидирует все cache entries для данного тега
- [ ] Все существующие тесты проходят
- [ ] Нет регрессий в загрузке уровней, материалов, текстур
