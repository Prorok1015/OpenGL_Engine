# Пошаговая инициализация фреймворка

Следуйте этим шагам по порядку.

---

## Шаг 1: Создание структуры (2 минуты)

```bash
# В корне вашего проекта выполните:

mkdir -p docs/agent/learning
mkdir -p docs/human
```

## Шаг 2: Копирование самообучения (1 минута)

```bash
# Скопируйте 7 файлов self-learning (они переиспользуются)
cp claude-framework/docs-template/agent/learning/* docs/agent/learning/

# Проверьте что скопировались:
ls docs/agent/learning/
# Должно быть 7 файлов .toon
```

## Шаг 3: Копирование CLAUDE.md и MEMORY_RULES.md (2 минуты)

```bash
# Скопируйте шаблон CLAUDE.md
cp claude-framework/CLAUDE_TEMPLATE.md CLAUDE.md

# Скопируйте шаблон правил памяти
cp claude-framework/MEMORY_RULES_TEMPLATE.md MEMORY_RULES.md

# Отредактируйте оба файла для вашего проекта
# CLAUDE.md:
#   - Замените [PROJECT_NAME] на название
#   - Обновите компоненты
#   - Обновите правила
#
# MEMORY_RULES.md:
#   - Замените [PROJECT_NAME] на название
#   - Убедитесь что понимаете когда сохранять в память
```

### Что отредактировать в CLAUDE.md

```markdown
# CLAUDE.md — [ВАШЕ_НАЗВАНИЕ_ПРОЕКТА]

## 🎯 Проект
**[НАЗВАНИЕ]** — [1-2 предложения о проекте]

## 📂 Структура
- **[Компонент 1]** — Что это делает
- **[Компонент 2]** — Что это делает
- **[Компонент 3]** — Что это делает

## ⚠️ Критические правила

1. **[Самое важное ограничение для вашего проекта]**
2. **[Второе важное]**
3. **[Третье важное]**

## 🗣️ Соглашения
- Язык кода: [ваш язык]
- Стиль: [ваш стиль]
- Комментарии: [на каком языке]
```

Минимум 300 слов, максимум 500 слов.

## Шаг 4: Создание .claudesignore (1 минута)

```bash
# Создайте файл
cat > .claudesignore << 'EOF'
# Build artifacts
build/
dist/
out/
node_modules/
__pycache__/
venv/

# IDE caches
.vs/
.vscode/
.idea/

# Keep human docs separate
docs/human/

# Generated
*.log
*.tmp
EOF
```

## Шаг 5: Создание базовых TOON файлов (5 минут)

Создайте эти 5 файлов в `docs/agent/`:

### docs/agent/README.toon

```toon
[DOCUMENTATION_INDEX]
format: TOON
audience: Claude agents working on [YOUR_PROJECT]

[FILES]
- PROJECT.toon → Project overview
- ARCHITECTURE.toon → System design
- CONSTRAINTS.toon → Rules and limits
- QUICK_LINKS.toon → File navigation

[USAGE]
Before working:
1. Read PROJECT.toon
2. Check CONSTRAINTS.toon (STRICT!)
3. Use QUICK_LINKS.toon to find files

[LEARNING]
Update documentation:
→ See learning/SELF-LEARNING.toon
```

### docs/agent/PROJECT.toon

```toon
[PROJECT]
name: [YOUR_PROJECT_NAME]
type: [Type: web, library, game, etc.]
description: [1-2 sentences]

[MODULES]
[MODULE.1]
name: [Component name]
responsibility: [What it does]

[MODULE.2]
name: [Another component]
responsibility: [What it does]

[STATUS]
current: [What phase]
next: [What's next]
```

### docs/agent/ARCHITECTURE.toon

```toon
[ARCHITECTURE]
pattern: [Your architecture pattern]

[COMPONENT_1]
name: [Component name]
responsibility: [What it owns]
depends_on: [Dependencies]

[CONSTRAINTS]
rule: [Your main rule 1]
rule: [Your main rule 2]
```

### docs/agent/CONSTRAINTS.toon

```toon
[CONSTRAINTS]
severity: CRITICAL

[CONSTRAINT_1]
rule: [Your first important rule]
why: [Why this matters]

[CONSTRAINT_2]
rule: [Your second important rule]
why: [Why this matters]
```

### docs/agent/QUICK_LINKS.toon

```toon
[QUICK_LINKS]
purpose: Fast file lookup

[CODE]
Component 1 → [path/to/component1]
Component 2 → [path/to/component2]

[DOCUMENTATION]
Overview → PROJECT.toon
Design → ARCHITECTURE.toon
Rules → CONSTRAINTS.toon
```

## Шаг 6: Перемещение старой документации (опционально)

Если у вас есть существующая документация:

```bash
# Переместите в docs/human/
mv docs/README.md docs/human/
mv docs/ARCHITECTURE.md docs/human/
# и т.д.
```

## Шаг 7: Проверка структуры (1 минута)

```bash
# Проверьте что всё создалось:
tree -L 2 docs/

# Должно быть:
# docs/
# ├── agent/
# │   ├── README.toon
# │   ├── PROJECT.toon
# │   ├── ARCHITECTURE.toon
# │   ├── CONSTRAINTS.toon
# │   ├── QUICK_LINKS.toon
# │   └── learning/
# │       └── [7 .toon files]
# └── human/
#     └── [ваша старая документация, если есть]
```

## Шаг 8: Добавление в Git (1 минута)

```bash
# Добавьте файлы
git add CLAUDE.md .claudesignore docs/

# Коммитьте
git commit -m "docs: initialize agent documentation framework"

# Опционально удалите папку claude-framework если она больше не нужна
rm -rf claude-framework/
```

## ✅ Финальная проверка

Убедитесь что у вас есть:

```
project-root/
├── CLAUDE.md                    ✅ Основные правила
├── MEMORY_RULES.md              ✅ Управление памятью между сессиями
├── .claudesignore               ✅ Исключает docs/human/ и артефакты
└── docs/
    ├── agent/                   ✅ Для Claude
    │   ├── README.toon
    │   ├── PROJECT.toon
    │   ├── ARCHITECTURE.toon
    │   ├── CONSTRAINTS.toon
    │   ├── QUICK_LINKS.toon
    │   └── learning/            ✅ 7 self-learning файлов
    │
    └── human/                   ⭐ Опционально (если есть старая доку)
```

---

## 🎉 Готово!

Теперь ваш проект готов для работы с Claude агентами:

```bash
# Просто используйте:
# 1. Покажите Claude ваш проект
# 2. Claude прочитает CLAUDE.md
# 3. Claude загрузит документацию из docs/agent/
# 4. Claude начнёт работать эффективно
# 5. Claude будет обновлять документацию при обнаружении правил
```

---

## ⏱️ Итого время: < 15 минут

- Шаг 1: 2 минуты (создание структуры)
- Шаг 2: 1 минута (копирование)
- Шаг 3: 3 минуты (CLAUDE.md)
- Шаг 4: 1 минута (.claudesignore)
- Шаг 5: 5 минут (TOON файлы)
- Шаг 6: 1 минута (старая доку)
- Шаг 7: 1 минута (проверка)
- Шаг 8: 1 минута (git)

**ИТОГО: < 15 минут**

---

## 🆘 Проблемы?

**"Не знаю, что написать в TOON файлы"**
→ Посмотрите примеры в Mirage проекте или читайте QUICK_START_NEW_PROJECT.md

**"Структура не совпадает"**
→ Еще раз выполните Шаги 1-2

**"Не уверен что это правильно"**
→ Просто покажите Claude — он поправит если что-то не так

**"Хочу понять систему лучше"**
→ Прочитайте FRAMEWORK_README.md или PROJECT_MANAGEMENT_RULES.md

---

## Next

После инициализации:
1. Прочитайте `CLAUDE.md` в вашем проекте
2. Используйте с Claude
3. Claude будет самообучаться! 🚀
