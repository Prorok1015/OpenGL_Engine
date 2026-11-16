# Записи об изменениях ABI игровых модулей и модулей редактора

Каждое изменение ABI кладёт сюда свой файл `<номер issue>.md`, например `993.md`. Два PR пишут разные файлы, поэтому записи не конфликтуют (#1056). Раньше запись была строкой в общем комментарии над `SNK_MODULE_API_VERSION`, и почти каждые два PR конфликтовали на ней. Та история теперь лежит в [`../HISTORY.md`](../HISTORY.md).

## Когда запись нужна
`tools/ci/check_abi_changes.py` (джоб Plan) требует запись, если PR:
- меняет `gs::detail::layout_revision` (`gs_module_layout.h`);
- меняет `SNK_MODULE_API_VERSION` или `SNK_EDITOR_API_VERSION`;
- меняет `tools/ci/vtable_pins.json`;
- добавляет строку с `SNK_API` или `SNK_EDITOR_API` в заголовок под `include/`.

Новый экспорт внутри класса, который уже `SNK_API`, скрипт не видит. Запись для него автор пишет сам, по правилу «Module ABI» из `CLAUDE.md`.

## Что в записи
Одна-пять строк на английском, в стиле прежней истории:
- что меняется для модуля: новые экспорты, поле в конце типа, тип в ключе раскладки, vtable;
- `layout_revision <N>`, если ревизия меняется (скрипт проверяет, что указано новое значение), или «no type changes: the layout key stays»;
- что нужно модулю: «a module that calls X needs an engine with #N».

Пример (`993.md`):

```text
#993: scn::prefab_instance gains content_of (the file its node is content of) at its end (layout_revision 14);
scn::reconcile and scn::expand_instance gain overloads that name that file, the old exports stay.
```

## К релизу
Когда окно 4.0 закроется или выйдет релиз, записи переносятся в `../HISTORY.md` под заголовком версии, а их файлы удаляются. Это делается одним PR.
