# Claude Framework — Инициализация

**Универсальный фреймворк для управления проектами с Claude AI**

Эта папка содержит всё необходимое для быстрой настройки документации вашего проекта.

---

## ⚡ Быстрая инициализация (< 10 минут)

### 1️⃣ Скопируйте папку в ваш проект

```bash
# Скопируйте эту папку в корень вашего проекта
cp -r claude-framework /path/to/your/project/

cd /path/to/your/project
```

### 2️⃣ Запустите инициализацию

```bash
# Выполните инициализацию (создаст структуру)
bash claude-framework/init.sh
```

Или вручную:

```bash
# 1. Скопируйте шаблон docs
cp -r claude-framework/docs-template docs/agent

# 2. Скопируйте CLAUDE.md шаблон
cp claude-framework/CLAUDE_TEMPLATE.md CLAUDE.md

# 3. Создайте .claudesignore
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

### 3️⃣ Кастомизируйте для вашего проекта

```bash
# Отредактируйте CLAUDE.md
# - Замените [PROJECT_NAME] на название вашего проекта
# - Обновите компоненты/правила
# - Сохраните

# Отредактируйте MEMORY_RULES.md
# - Замените [PROJECT_NAME] на название
# - Убедитесь что агент понимает когда сохранять в память
# - Это критично для кроссессийного обучения
```

### 4️⃣ Готово! 🚀

```bash
# Добавьте в git
git add CLAUDE.md .claudesignore docs/
git commit -m "docs: initialize agent documentation framework"
```

---

## 📂 Что находится в этой папке

```
claude-framework/
├── README.md                          ← Вы здесь
├── INIT.md                            ← Пошаговая инициализация
├── init.sh                            ← Автоматический скрипт
│
├── FRAMEWORK_README.md                ← Полный обзор фреймворка
├── PROJECT_MANAGEMENT_RULES.md        ← Философия и детали
├── QUICK_START_NEW_PROJECT.md         ← Детальный гайд
├── CLAUDE_TEMPLATE.md                 ← Шаблон CLAUDE.md
├── MEMORY_RULES_TEMPLATE.md           ← Шаблон MEMORY_RULES.md
│
└── docs-template/                     ← Шаблон структуры
    └── agent/
        └── learning/
            ├── SELF-LEARNING.toon
            ├── WHEN_TO_UPDATE.toon
            ├── WHEN_NOT_TO_UPDATE.toon
            ├── FILES_REFERENCE.toon
            ├── HOW_TO_UPDATE.toon
            ├── EXAMPLES.toon
            └── GUARDRAILS.toon
```

---

## 📖 Какой файл читать?

### Хочу быстро настроить (5 минут)
→ Следуйте **этому файлу** (README.md) → Раздел "Быстрая инициализация"

### Нужны пошаговые инструкции (10 минут)
→ Откройте **INIT.md** (детальное руководство)

### Хочу понять систему (15 минут)
→ Читайте **FRAMEWORK_README.md** (полный обзор)

### Нужны все детали (30 минут)
→ Читайте **PROJECT_MANAGEMENT_RULES.md** (полная архитектура)

### Нужна развёрнутая инструкция (20 минут)
→ Читайте **QUICK_START_NEW_PROJECT.md** (подробный гайд)

### Нужно кастомизировать CLAUDE.md
→ Скопируйте **CLAUDE_TEMPLATE.md** как CLAUDE.md и отредактируйте

---

## ✅ Чек-лист инициализации

- [ ] Скопировал claude-framework/ в корень проекта
- [ ] Выполнил инициализацию (вручную или скриптом)
- [ ] Отредактировал CLAUDE.md для своего проекта
- [ ] Создал .claudesignore
- [ ] Все файлы добавлены в git
- [ ] Структура docs/agent/ создана
- [ ] Файлы self-learning в docs/agent/learning/ скопированы

**Всё отмечено?** → Готово к использованию! 🎉

---

## 🎯 После инициализации

### Что делать дальше

1. **Создайте специфичные для проекта TOON файлы**
   ```
   docs/agent/PROJECT.toon
   docs/agent/ARCHITECTURE.toon
   docs/agent/CONSTRAINTS.toon
   docs/agent/QUICK_LINKS.toon
   ```

2. **Используйте фреймворк в работе с Claude**
   ```
   Готово! Агенты могут теперь:
   - Читать документацию
   - Находить нужную информацию
   - Обновлять доку при обнаружении правил
   - Самообучаться между сессиями
   ```

3. **Совет**: Используйте docs/agent/learning/ файлы для обновления документации
   - Агенты будут их читать при необходимости обновить доку
   - Файлы проекта-агностичны, их можно переиспользовать

---

## 🚀 Использование в других проектах

### Для нового проекта

```bash
# 1. Клонируйте или скопируйте claude-framework
cp -r /path/to/claude-framework /your/new/project/

# 2. Инициализируйте
cd /your/new/project
bash claude-framework/init.sh

# 3. Кастомизируйте CLAUDE.md
# 4. Готово!
```

### Для существующего проекта

```bash
# 1. Скопируйте only структуру
cp -r claude-framework/docs-template docs/agent

# 2. Скопируйте шаблон CLAUDE.md
cp claude-framework/CLAUDE_TEMPLATE.md CLAUDE.md

# 3. Создайте .claudesignore

# 4. Переместите старые доксы (опционально)
mkdir docs/human
mv docs/* docs/human/  # кроме docs/agent

# 5. Кастомизируйте и готово!
```

---

## 📋 Структура после инициализации

Ваш проект будет выглядеть так:

```
your-project/
├── CLAUDE.md                    (< 500 слов - основные правила)
├── MEMORY_RULES.md              (кроссессийное обучение)
├── .claudesignore               (исключает docs/human/ и артефакты)
├── README.md                    (о проекте)
│
├── claude-framework/            (этот фреймворк, можно удалить после настройки)
│
└── docs/
    ├── agent/                   (для Claude - компактно, TOON формат)
    │   ├── README.toon
    │   ├── PROJECT.toon
    │   ├── ARCHITECTURE.toon
    │   ├── CONSTRAINTS.toon
    │   ├── [YOUR_DOMAIN.toon]
    │   │
    │   └── learning/            (самообучение - переиспользуется!)
    │       ├── SELF-LEARNING.toon
    │       ├── WHEN_TO_UPDATE.toon
    │       └── [5 more files]
    │
    └── human/                   (для людей - полные детали, опционально)
        ├── README.md
        ├── ARCHITECTURE.md
        └── [YOUR_STRUCTURE]/
```

---

## 🎯 Три способа инициализации

### Способ 1: Автоматический (самый быстрый)
```bash
bash claude-framework/init.sh
# Всё создаст автоматически
```

### Способ 2: Вручную с инструкциями (понятнее)
```bash
# Следуйте инструкциям в INIT.md
```

### Способ 3: Пошагово (для понимания)
```bash
# Прочитайте QUICK_START_NEW_PROJECT.md
# Выполняйте каждый шаг вручную
```

---

## ❓ Частые вопросы

**Q: Можно ли удалить папку claude-framework после инициализации?**
A: Да! Её можно удалить, всё нужное скопировано в проект. Или оставьте как reference.

**Q: Что если у меня уже есть документация?**
A: Переместите в `docs/human/`, `docs/agent/` будет для Claude.

**Q: Как кастомизировать для моего проекта?**
A: Отредактируйте `CLAUDE.md` и создайте `docs/agent/*.toon` файлы для вашего домена.

**Q: Что делать с self-learning файлами?**
A: Они переиспользуются для всех проектов. Агенты будут их читать при обновлении доку.

**Q: Как это работает с Claude?**
A: Просто покажите агенту ваш проект → Он прочитает CLAUDE.md и docs/agent/ → Будет работать эффективно.

---

## 📞 Быстрая помощь

**Не знаю, с чего начать?**
→ Выполните 4 шага выше (< 10 минут)

**Хочу понять систему?**
→ Прочитайте FRAMEWORK_README.md

**Нужны пошаговые инструкции?**
→ Откройте INIT.md

**Нужна помощь с кастомизацией?**
→ Смотрите QUICK_START_NEW_PROJECT.md

---

## 🎉 Готово!

После инициализации ваш проект будет иметь:
- ✅ Компактный CLAUDE.md (основные правила)
- ✅ Модульную документацию для Claude (docs/agent/)
- ✅ Самообучение для агентов (docs/agent/learning/)
- ✅ Эффективный контекст (75% экономия токенов)
- ✅ Растущую базу знаний (улучшается каждую сессию)

**Начните работу!** 🚀

---

**Framework Version**: 1.0 Stable  
**Created**: April 2026  
**License**: MIT (use freely)

Next: Читайте **INIT.md** для пошаговой инициализации или выполните скрипт выше!
