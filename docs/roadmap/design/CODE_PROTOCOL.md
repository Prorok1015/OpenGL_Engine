# Протокол `code://` и дерево «Code» (US-52-M, #450)

**Issue:** #450 · **Эпик:** #21 (EPIC-52)

Исходники игрового модуля — ресурсы со своим тегом. Это не `res://`: код не импортируется, не готовится и не получает дескрипторов. `res://` не видит код, а `code://` не видит ассеты.

## Монтирование
- `code://` смонтирован на `<project>/code`, только если папка существует (иначе протокола нет, раздел «Code» не рисуется, кадр smoke не меняется).
- `prj::verify_project_mounts` проверяет `code/`: настоящая папка (не symlink и не junction), внутри проекта, не пересекается с content roots и с `.cache`, `.snk`, `build`.
- Тексты читаются как `res::text_resource`.

## Что считается кодом (`snk/project/prj_code.h`)
Единственный источник правды для политики записи, watcher, дерева и политики агента (A6c, #449).
- `prj::code_category_of(path)`: `source` (`.cpp .cc .cxx`), `header` (`.h .hpp .inl`), `cmake` (`CMakeLists.txt`, `*.cmake`, `CMakePresets.json`, `CMakeUserPresets.json`), иначе `other`. Правка CMake-файла исполняет код при configure, поэтому это отдельная категория.
- `prj::is_code_ignored_dir(name)`: `build`, `out`, `_deps`, `CMakeFiles` и любое имя с точки (`.git`, `.cache`, `.snk`). Регистр не важен.
- `prj::code_write_patterns()`: разрешающие шаблоны по расширениям и запрещающие `!code://<папка>/**` и `!code://**/<папка>/**` для игнорируемых папок.

## Запись
- Только через `resource_system::store` и `write_policy` поверх безопасной записи #427 (`openat`/`O_NOFOLLOW`, `NtCreateFile` от дескриптора родителя). Выход по `..`, symlink и junction отклоняется; `build/` и `.cache/` лежат вне `code/`.
- `res::write_policy` понимает шаблоны `!protocol://glob`: запрет побеждает разрешение независимо от порядка. Политика `code://` сворачивает регистр (`case_sensitive=false`), чтобы `Build/a.cpp` на Windows и macOS не обходил запрет на `build/`.
- Удаление и переименование: `resource_system::remove(tag)` / `rename(from, to)` (`mount.file_operations = true`, только `code://`). Только регулярные файлы, обе стороны проходят политику, перенос в пределах одного mount, существующий файл не перезаписывается (`renameat2 RENAME_NOREPLACE` на Linux, переименование без `ReplaceIfExists` на Windows). Папка назначения должна существовать. Регистровое переименование (`a.cpp` → `A.cpp`) разрешено (сравнивается идентичность файла).
- Создание нового файла (`resource_system::create_sync`) никогда не заменяет существующий, в том числе созданный IDE во время записи (временный файл переносится без замены).
- Имена на `code://` проверяются как переносимые на всех платформах (`res::portable_path_problem`): без `:` (потоки NTFS), `.`/`..`, пробела или точки в конце, имён устройств Windows (CON, NUL, COM1...), управляющих символов.

## События
Один watcher на mount `code://` (тот же `file_watcher` внутри `vfs_resolver`, опрос раз в 0,5 с, без прохода при отсутствии mount).
```cpp
resource_system::subscribe_changes("code", owner, [](std::span<const res::file_change> batch) { ... });
struct file_change { res::tag tag; res::tag old_tag; change_kind kind; bool is_directory; bool self; };
// kind: created | modified | deleted | renamed
```
- Пачка на опрос (или на операцию), на главном потоке, в порядке: renamed, deleted (дети раньше родителей), created (родители раньше детей), modified. Дальнейший debounce и коалесинг на стороне подписчика (live-режим #445).
- `self = true` для того, что сделано через этот `resource_system` (store, remove, rename); внешние изменения (IDE, агент с доступом к диску) приходят с `self = false`.
- `renamed`: исчез один файл и появился один с тем же размером и временем изменения за один опрос. Если кандидатов больше одного, приходят `deleted` и `created`, а не догадка.
- Внешнее удаление или переименование сбрасывает кэш затронутых тегов, как и создание или изменение.
- Имена файлов везде UTF-8 (`fs_paths::utf8_of`), не `path::string()`: на MSVC он бросает на имени вне кодовой страницы.
- Watcher не заходит в игнорируемые папки (тот же список, что и запрет записи), не следует по ссылкам и сообщает только о файлах кода.

## Дерево «Code» в Asset Browser
`edt_code_tree.{h,cpp}` (модель и операции без GUI, покрыты тестами) и `edt_code_section.{h,cpp}` (ImGui). Двойной клик открывает файл через `platform::open_in_shell` (ShellExecuteExW / `/usr/bin/open` / `xdg-open`, только argv; на Linux отсутствие обработчика не видно сразу). Контекстное меню: New file, Rename, Delete (с подтверждением), Open, Copy path. Дерево обновляется по подписке на события. Создание папок отдельной операцией пока нет: подпапки создаются вместе с файлом (`ai/brain/mind.h`).
