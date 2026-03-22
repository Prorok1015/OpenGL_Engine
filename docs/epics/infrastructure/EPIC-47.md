# EPIC-47: ds::string_id — хешированные идентификаторы строк

**Status:** planned
**Theme:** infrastructure
**Dependencies:** none

---

## Мотивация

Движок повсеместно использует `std::string` для идентификаторов: имена костей (skinning), имена анимаций, имена ассет-нодов, имена типов дескрипторов. Это неэффективно по нескольким причинам:

- **Память** — каждая строка (даже короткая вроде "Armature.001") занимает ~48 байт на 64-bit системе (указатель, size, capacity) + сама строка.
- **Сравнения** — поиск по имени в `unordered_map<std::string, T>` требует хеширования полной строки при каждом обращении.
- **Debug vs Release** — в Release-сборке многие строки можно вообще выбросить, оставив только хеш.

### Стратегия

1. Реализовать `ds::string_id` — компактный тип, хранящий `uint32_t` хеш
2. В Debug-сборке: хранить оригинальную строку для читаемости логов
3. В Release-сборке: только хеш (4 байта вместо 48+N)
4. Сравнение — всегда O(1) по хешу
5. Поддержка как ключа в `unordered_map` с кастомным hasher'ом
6. Постепенная миграция строк по всему движку

### Хеш-функция

Использовать **FNV-1a** (Fowler-Noll-Vo):
- Простая и быстрая
- `constexpr` для compile-time хеширования
- 32-bit хешей достаточно для типичного количества идентификаторов в игре

## Архитектурное решение

### Новые файлы

```
core/core/common/ds_string_id.hpp           ← header-only, FNV-1a + string_id класс
core/core/common/ds_string_id.cpp           ← (если нужна глобальная таблица для debug)
core/core/common/ds_string_id_tests.cpp     ← unit tests (в unittests/)
```

### Дизайн string_id

```cpp
namespace ds {
	// FNV-1a констексп хеш-функция
	constexpr uint32_t fnv1a_hash(std::string_view str) noexcept;

	// Debug-режим: хранит и хеш, и строку для читаемости
	// Release-режим: только хеш
	class string_id {
	public:
		// Конструкторы
		constexpr string_id() noexcept = default;
		explicit string_id(std::string_view str) noexcept;

		// Доступ к хешу (всегда доступен)
		constexpr uint32_t hash() const noexcept { return hash_; }

		// Доступ к строке (только Debug)
		#ifndef NDEBUG
			std::string_view str() const noexcept { return str_; }
		#endif

		// Операторы
		bool operator==(const string_id& other) const noexcept;
		bool operator!=(const string_id& other) const noexcept;

		// Конверсия
		explicit operator uint32_t() const noexcept { return hash_; }
		explicit operator bool() const noexcept { return hash_ != 0; }

	private:
		uint32_t hash_ = 0;
		#ifndef NDEBUG
			std::string str_;
		#endif
	};
}

// Специализация std::hash для unordered_map
namespace std {
	template <>
	struct hash<ds::string_id> {
		size_t operator()(const ds::string_id& id) const noexcept {
			return static_cast<size_t>(id.hash());
		}
	};

	// Поддержка std::formatter для форматирования
	template <>
	struct formatter<ds::string_id> {
		// В Release: выводит только хеш
		// В Debug: выводит "имя (0xhash)"
	};
}
```

### Пример использования

```cpp
// Before:
std::unordered_map<std::string, Animation> animations;
animations["run"] = ...;
if (animations.find("walk") != animations.end()) { ... }

// After:
std::unordered_map<ds::string_id, Animation> animations;
animations[ds::string_id("run")] = ...;
if (animations.find(ds::string_id("walk")) != animations.end()) { ... }

// Compile-time:
constexpr auto id = ds::string_id(ds::fnv1a_hash("static_bone_name"));
```

### Интеграция с exists системами

Все места, где используются строки как идентификаторы, будут постепенно переходить на `string_id`:

- Bone names in `skinning_component`
- Animation names in `animation_component`, asset descriptors
- Node names in `prefab_node`, scene hierarchy
- Type names in descriptor deserialization (`desc_builder.cpp`)

---

## User Stories

### US-47-1: FNV-1a констексп хеш-функция
**Файлы:** `core/core/common/ds_string_id.hpp`

Реализовать `ds::fnv1a_hash(std::string_view) constexpr` для compile-time и runtime хеширования.

**AC:**
- [ ] Функция `constexpr` и работает в compile-time контексте
- [ ] Основана на FNV-1a алгоритме с 32-bit хешом
- [ ] Работает корректно для пустой строки (возвращает начальное смещение FNV)
- [ ] Детерминирована — одна и та же строка всегда дает один хеш
- [ ] Unit-тесты: пустая строка, простые имена, длинные строки, спецсимволы

### US-47-2: string_id класс (Debug + Release версии)
**Файлы:** `core/core/common/ds_string_id.hpp`

Реализовать `ds::string_id` с хранением хеша в обоих режимах и оригинальной строки в Debug.

**AC:**
- [ ] Конструктор по умолчанию инициализирует пустой ID (hash = 0)
- [ ] Конструктор из `std::string_view` вычисляет FNV-1a хеш
- [ ] `hash()` const возвращает uint32_t
- [ ] В Debug: `str()` const возвращает исходную строку
- [ ] В Release: нет поля для строки (sizeof(string_id) == 4)
- [ ] Оператор сравнения `==` / `!=` сравнивает только хеши
- [ ] Операторы преобразования: `explicit operator uint32_t()`, `explicit operator bool()`
- [ ] Unit-тесты: создание, сравнение, Debug логирование

### US-47-3: std::hash специализация и форматирование
**Файлы:** `core/core/common/ds_string_id.hpp`

Добавить поддержку `string_id` в `std::hash` для использования в `unordered_map` и `std::formatter` для логирования.

**AC:**
- [ ] `std::hash<ds::string_id>` специализирована и работает в unordered_map
- [ ] `std::formatter<ds::string_id>` реализует форматирование
  - [ ] Release: формат `{0xhash}` (например `{0x1a2b3c4d}`)
  - [ ] Debug: формат `name (0xhash)` (например `run (0x1a2b3c4d)`)
- [ ] Работает с `std::format()` и логгером (spdlog)
- [ ] Unit-тесты: форматирование в обоих режимах, вывод в логи

### US-47-4: Миграция bone names (skinning_component)
**Файлы:** `core/engine/scene/components/scn_skinning_component.h`, related loaders

Заменить `std::string` на `ds::string_id` для имён костей в `skinning_component`.

**AC:**
- [ ] `skinning_component::bone_transforms` использует `std::vector<ds::string_id>` вместо `std::vector<std::string>`
- [ ] Loader (GLTF/Assimp) создает `string_id` из имён костей при импорте
- [ ] Lookup по имени кости — O(1) через unordered_map с `string_id` ключом
- [ ] Unit-тесты: загрузка модели с костями, поиск по имени кости

### US-47-5: Миграция animation names (animation_component)
**Файлы:** `core/engine/scene/components/scn_animation_component.h`, `prefab_desc`

Заменить `std::string` на `ds::string_id` для имён анимаций.

**AC:**
- [ ] `animation_component::clips` ключи — `ds::string_id`
- [ ] `prefab_desc` сохраняет и загружает animation names как `string_id`
- [ ] Дескриптор при десериализации конвертирует строки в `string_id`
- [ ] Unit-тесты: сохранение/загрузка описаний с анимациями

### US-47-6: Миграция node names (prefab_node, scene hierarchy)
**Файлы:** `core/engine/desc/prefab_desc.h`, `core/engine/scene/components/scn_hierarchy_component.h`

Заменить `std::string` на `ds::string_id` для имён ассет-нодов и иерархии сцены.

**AC:**
- [ ] `prefab_node::name` — `ds::string_id`
- [ ] `hierarchy_component` тег узла — `ds::string_id`
- [ ] Поиск ноды по имени — O(1) через unordered_map
- [ ] Unit-тесты: создание иерархии, поиск по имени

### US-47-7: Миграция type names (desc десериализация)
**Файлы:** `core/core/desc/desc_builder.cpp`, related descriptor loaders

Заменить `std::string` на `ds::string_id` для типов компонентов в дескрипторах.

**AC:**
- [ ] `desc_builder` при десериализации JSON конвертирует type names в `string_id`
- [ ] Registry типов может быть заиндексирован по `string_id` ключам
- [ ] Логирование ошибок неизвестного типа выводит читаемое имя (Debug)
- [ ] Unit-тесты: загрузка описаний с неизвестными типами, вывод в логи

---

## Порядок выполнения

```
US-47-1 (FNV-1a hash function)
  └── US-47-2 (string_id class)
        └── US-47-3 (std::hash + formatter)
              ├── US-47-4 (bone names migration)
              ├── US-47-5 (animation names migration)
              ├── US-47-6 (node names migration)
              └── US-47-7 (desc type names migration)

(US-47-4 через US-47-7 могут выполняться параллельно после US-47-3)
```

## Риски

- **Hash collisions** — с 32-bit хешом вероятность коллизии растет с количеством идентификаторов. Для типичной игры (сотни костей, анимаций) крайне маловероятно, но возможно. Митигация: можно переключиться на 64-bit хеш если понадобится.
- **Отладка** — Release-сборка без оригинальных строк сложнее для отладки. Митигация: Debug-сборки сохраняют строки; при отправке крашей включать Debug символы.
- **Миграция покрытия** — много мест используют строки идентификаторов. Mitigation: миграция поэтапная, старые пути работают, пока не переписаны.

## Критерии завершения эпика

- [ ] `string_id` класс и FNV-1a хеш реализованы, покрыты unit-тестами
- [ ] `std::hash` и `std::formatter` поддерживают `string_id`
- [ ] Bone names, animation names, node names, type names полностью мигрированы на `string_id`
- [ ] Внутренний профайлер подтверждает уменьшение потребления памяти и улучшение производительности lookup'ов
- [ ] Debug-сборки содержат оригинальные строки для читаемости логов
