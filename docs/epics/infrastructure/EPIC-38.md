# EPIC-38: res::tag — std::formatter и ostream support

**Status:** done
**Theme:** infrastructure
**Dependencies:** none

---

## Мотивация

`res::tag` — основной идентификатор ресурсов, используется повсеместно в логах, debug-выводе и тестах. Сейчас для вывода нужно вызывать `.string()` вручную. Отсутствие `operator<<` делает тег non-printable в Boost.Test (тесты вынуждены использовать `BOOST_CHECK` вместо `BOOST_TEST`).

Нужен `std::formatter<res::tag>` (C++20) с поддержкой спецификаторов формата для вывода подстрок, а также `operator<<` для обратной совместимости с ostream/Boost.Test.

## Архитектурное решение

### Формат-спецификаторы

| Спецификатор | Вывод | Пример для `res://models/hero.glb` |
|---|---|---|
| `{}` (пустой) | полная строка | `res://models/hero.glb` |
| `{:name}` | `tag.name()` | `hero.glb` |
| `{:path}` | `tag.path()` | `models` |
| `{:ext}` | `tag.extension()` | `glb` |
| `{:pure}` | `tag.pure_name()` | `hero` |
| `{:proto}` | `tag.protocol()` | `res` |
| `{:rel}` | `tag.relative()` | `models/hero.glb` |

### Реализация

```cpp
// В res_tag.h, после определения класса tag, перед закрытием namespace res:

// ostream support (для Boost.Test printability и legacy)
inline std::ostream& operator<<(std::ostream& os, const tag& t) {
    return os << t.string();
}

// В глобальном namespace, после namespace res:
template<>
struct std::formatter<res::tag> {
    std::string_view spec;

    constexpr auto parse(std::format_parse_context& ctx) {
        auto it = ctx.begin();
        auto end = ctx.end();
        if (it != end && *it != '}') {
            auto spec_begin = it;
            while (it != end && *it != '}') ++it;
            spec = std::string_view(&*spec_begin, std::distance(spec_begin, it));
        }
        return it;
    }

    auto format(const res::tag& t, std::format_context& ctx) const {
        std::string_view value;
        if (spec.empty())      value = t.view();
        else if (spec == "name")  value = t.name();
        else if (spec == "path")  value = t.path();
        else if (spec == "ext")   value = t.extension();
        else if (spec == "pure")  value = t.pure_name();
        else if (spec == "proto") value = t.protocol();
        else if (spec == "rel")   value = t.relative();
        else                      value = t.view();
        return std::format_to(ctx.out(), "{}", value);
    }
};
```

## User Stories

### US-38-1: Реализовать std::formatter<res::tag> и operator<<
**Файлы:** `core/core/resource/res_tag.h`

Описание: Добавить `operator<<` для ostream и специализацию `std::formatter<res::tag>` с поддержкой спецификаторов.

**AC:**
- [x] `std::format("{}", tag)` выводит полную строку тега
- [x] `std::format("{:name}", tag)` выводит имя файла
- [x] `std::format("{:path}", tag)` выводит путь
- [x] `std::format("{:ext}", tag)` выводит расширение
- [x] `std::format("{:pure}", tag)` выводит имя без расширения
- [x] `std::format("{:proto}", tag)` выводит протокол
- [x] `std::format("{:rel}", tag)` выводит relative path
- [x] `operator<<(ostream, tag)` работает (для Boost.Test и logging)
- [x] Unit-тесты для всех спецификаторов в `unittests/res_tag_formatter_tests.cpp`

### US-38-2: Обновить тесты — заменить BOOST_CHECK на BOOST_TEST для res::tag
**Файлы:** `unittests/res_tag_tests.cpp`, `unittests/rnd_shader_config_tests.cpp`

Описание: После добавления `operator<<` тег станет printable. Заменить `BOOST_CHECK` обратно на `BOOST_TEST` для лучшей диагностики при падениях.

**AC:**
- [x] `BOOST_CHECK(a == b)` → `BOOST_TEST(a == b)` в res_tag_tests.cpp
- [x] `BOOST_CHECK(config == other)` → `BOOST_TEST(config == other)` в rnd_shader_config_tests.cpp
- [x] Все тесты проходят

---

## Порядок выполнения

```
US-38-1 (formatter + operator<<)
  └── US-38-2 (обновить тесты)
```

## Критерии завершения эпика

- [x] `std::format("{:name}", tag)` и другие спецификаторы работают
- [x] `operator<<` работает в Boost.Test и logging
- [x] Unit-тесты покрывают все спецификаторы
- [ ] Все существующие тесты проходят
