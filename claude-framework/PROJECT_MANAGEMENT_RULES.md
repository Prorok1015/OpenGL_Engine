# Universal Project Management Framework for Claude AI

**Version**: 1.0  
**Purpose**: Optimize Claude agent work through modular documentation and context efficiency  
**Applicability**: Any software project (C++, Python, Go, JavaScript, etc.)

---

## 🎯 Core Principles

### 1. Documentation is Code
- Documentation is not optional; it's a project asset
- Changes to docs are tracked in git like code
- Knowledge accumulates across agent sessions

### 2. Context Efficiency
- Avoid huge monolithic documents
- Load only what's needed (modular approach)
- Measure: token budget for documentation

### 3. Agent Self-Learning
- Agents update documentation when discovering rules
- Each session improves knowledge for next session
- Explicit criteria prevent documentation bloat

### 4. Clear Separation of Audiences
- **Human docs** (`docs/human/`): Full details, complete context
- **Agent docs** (`docs/agent/`): Compact TOON format, quick lookup

---

## 📁 Recommended Directory Structure

```
project/
├── CLAUDE.md                              # Main rules (keep small!)
├── .claudesignore                         # Exclude unnecessary files
├── PROJECT_MANAGEMENT_RULES.md            # This file (copy to new projects)
│
└── docs/
    ├── agent/                             # For Claude (compact)
    │   ├── README.toon                    # Navigation
    │   ├── PROJECT.toon                   # Project overview
    │   ├── ARCHITECTURE.toon              # System design
    │   ├── CONSTRAINTS.toon               # Rules & limits
    │   ├── MODULES.toon                   # Component details
    │   ├── [MORE_DOMAIN_SPECIFIC.toon]    # Your domain
    │   ├── ACTIVE_TASKS.toon              # Current work
    │   ├── QUICK_LINKS.toon               # File navigation
    │   │
    │   └── learning/                      # Self-learning (modular!)
    │       ├── SELF-LEARNING.toon
    │       ├── WHEN_TO_UPDATE.toon
    │       ├── WHEN_NOT_TO_UPDATE.toon
    │       ├── FILES_REFERENCE.toon
    │       ├── HOW_TO_UPDATE.toon
    │       ├── EXAMPLES.toon
    │       └── GUARDRAILS.toon
    │
    └── human/                             # For people (full details)
        ├── README.md
        ├── ARCHITECTURE.md
        ├── DESIGN/
        ├── REQUIREMENTS/
        └── [YOUR_STRUCTURE]/
```

---

## 📋 File Sizes and Token Budget

| File | Ideal Size | Loaded When | Tokens |
|------|-----------|-------------|--------|
| **CLAUDE.md** | 300-500 words | Always | ~150 |
| **docs/agent/*.toon** | 200-500 words each | By reference | ~2,000 total |
| **docs/agent/learning/\*.toon** | 200-400 words each | On documentation update | ~1,600 total |
| **docs/human/\*** | Unlimited | Referenced, not loaded | N/A |

**Target**: Agent context uses ~3,500 words for documentation (load by need)

---

## 🏗️ CLAUDE.md Template (Universal)

```markdown
# CLAUDE.md

## 🎯 Project

[1-2 sentence project description]
- Key technologies
- Main components
- Core principles

## 📂 Structure

- **Component A** — What it does
- **Component B** — What it does
- **Component C** — What it does

👉 [Full structure](PROJECT_STRUCTURE.md) | 🤖 [Agent docs](docs/agent/README.toon) | 👤 [Human docs](docs/human/)

## ⚠️ Critical Rules

1. [Most important rule for this project]
2. [Second most important rule]
3. [Third most important rule]

## 🤖 Self-Learning & Documentation Updates

Update docs when discovering rules → [docs/agent/learning/SELF-LEARNING.toon](docs/agent/learning/SELF-LEARNING.toon)

Quick guides:
- [When to update](docs/agent/learning/WHEN_TO_UPDATE.toon)
- [When NOT to update](docs/agent/learning/WHEN_NOT_TO_UPDATE.toon)
- [Which file to edit](docs/agent/learning/FILES_REFERENCE.toon)
- [Update process](docs/agent/learning/HOW_TO_UPDATE.toon)
- [Real examples](docs/agent/learning/EXAMPLES.toon)
- [Safety checks](docs/agent/learning/GUARDRAILS.toon)
```

---

## 📝 TOON Format Guidelines

**TOON** (Tree of Thought Ontology Notation) is a structured format for AI consumption:

```toon
[SECTION_NAME]
overview: Brief description
purpose: Why this section exists

[SUBSECTION]
key: value
description: Explain it

[ITEM]
name: Name of thing
rule: The rule or constraint
why: Reason/justification
when: When does this apply
example: Concrete example
```

**Benefits**:
- Structured (easy to parse)
- Compact (80% less than markdown for same info)
- Scannable (find info fast)
- Machine-friendly (better for AI)

---

## 🎯 Implementation Steps

### For Existing Project

1. **Create structure**
   ```bash
   mkdir -p docs/agent/learning docs/human
   ```

2. **Copy self-learning templates**
   - Copy all files from Mirage's `docs/agent/learning/`
   - These are project-agnostic (can reuse as-is)

3. **Create agent docs** (`docs/agent/`)
   - `README.toon` — Navigation for your domain
   - `PROJECT.toon` — Your project overview
   - `ARCHITECTURE.toon` — Your system design
   - `CONSTRAINTS.toon` — Your project rules
   - Domain-specific `.toon` files (MODULES, CRYPTO, API, etc.)

4. **Create CLAUDE.md**
   - Use template above
   - Keep it small (300-500 words)
   - Link to everything else

5. **Create .claudesignore**
   ```
   # Build artifacts
   build/
   dist/
   out/
   
   # Dependencies
   node_modules/
   venv/
   vendor/
   
   # Human documentation (use agent docs instead)
   docs/human/
   
   # IDE caches
   .vs/
   .vscode/
   .idea/
   ```

6. **Move existing docs**
   ```bash
   mv docs/* docs/human/  # Old docs to human/
   ```

### For New Project

1. **Use as template**: Copy entire `docs/` structure from Mirage
2. **Customize**: Replace PROJECT.toon, ARCHITECTURE.toon, etc. with your domain
3. **Self-learning**: Reuse `docs/agent/learning/` as-is (project-agnostic)
4. **Start fresh**: Initial agent session generates domain-specific docs

---

## 🤖 Agent Instructions (Use This in Chat)

You can copy/paste this prompt to Claude when starting a new project:

```
I'm starting a new [PROJECT_TYPE] project. 
Use this framework to generate the documentation structure:

1. Create docs/agent/README.toon (navigation)
2. Create docs/agent/PROJECT.toon (project overview)
3. Create docs/agent/ARCHITECTURE.toon (system design)
4. Create docs/agent/CONSTRAINTS.toon (project rules)
5. Create docs/agent/QUICK_LINKS.toon (file navigation)
6. Copy docs/agent/learning/* from the framework (project-agnostic)
7. Create CLAUDE.md (main rules, keep it small)
8. Create .claudesignore (exclude large build dirs and docs/human/)

Framework reference: [PASTE THE PROJECT_MANAGEMENT_RULES.md]

Project details:
- Name: [your project name]
- Type: [Python/C++/JavaScript/etc]
- Main components: [your components]
- Key technologies: [your tech stack]
- Critical rules: [your rules]
```

---

## 📊 Metrics & Optimization

### Token Budget Tracking
```
Session start:
- CLAUDE.md loaded: ~150 tokens
- Referenced agent docs loaded: ~2,000 tokens (on demand)
- Total documentation budget: ~3,500 tokens

vs.

Old approach:
- One huge CLAUDE.md: ~1,500 tokens
- All docs loaded: ~15,000+ tokens
- No navigation structure
```

### Success Criteria
- ✅ CLAUDE.md is quick to read (< 5 minutes)
- ✅ Each agent doc solves one problem (< 10 minutes each)
- ✅ Links between files work
- ✅ Agent can find what it needs without asking
- ✅ Documentation updates happen naturally (self-learning)

---

## 🔄 Documentation Update Workflow

### When Agent Discovers a Rule

1. **Check criteria** (docs/agent/learning/WHEN_TO_UPDATE.toon)
   - Is it from user? ✅
   - Found in code? ✅
   - Discovered through work? ✅
   - Fixes an error? ✅

2. **Find correct file** (docs/agent/learning/FILES_REFERENCE.toon)
   - Critical rule → CLAUDE.md
   - Architectural limit → CONSTRAINTS.toon
   - Algorithm detail → MODULES.toon
   - etc.

3. **Update** (docs/agent/learning/HOW_TO_UPDATE.toon)
   - Add 5-10 lines
   - Cite source
   - Check for duplicates

4. **Commit**
   ```bash
   git commit -m "docs: [type] brief description
   
   Source: [where from]
   File: [what file]
   
   Co-Authored-By: Claude <noreply@anthropic.com>"
   ```

### Result
Each session improves documentation → Next session has better context 🚀

---

## 🚫 Anti-Patterns (What NOT to Do)

❌ **One huge CLAUDE.md file**
- Creates token overhead
- Hard to find anything
- Discourages updates

❌ **No documentation for agents**
- Agents ask repeatedly for same info
- No institutional knowledge
- Context wasted on explanations

❌ **Monolithic docs (everything in one file)**
- Can't load what you need
- All-or-nothing context loading
- Harder to update

❌ **No self-learning mechanism**
- Rules stay tribal knowledge
- Each session starts from zero
- Knowledge decays

---

## 📚 Integration with Existing Projects

### For Projects with Git History

```bash
# Keep human docs
mv docs/ docs/human/

# Create agent structure
mkdir -p docs/agent/learning

# Copy this framework
cp PROJECT_MANAGEMENT_RULES.md /path/to/project/
cp -r docs/agent/learning /path/to/project/docs/agent/

# Create agent docs (start with template)
cp CLAUDE_TEMPLATE.md /path/to/project/CLAUDE.md
# Edit for your project

# Exclude human docs from Claude context
echo "docs/human/" >> /path/to/project/.claudesignore
```

---

## 🎓 Example: Applying to Different Domains

### Web Project (Python/Django)

**docs/agent/** would have:
- PROJECT.toon → Django app overview
- ARCHITECTURE.toon → MVC/MTV pattern
- CONSTRAINTS.toon → Django-specific rules
- API.toon → Endpoint documentation
- DATABASE.toon → Model descriptions
- TESTING.toon → Test strategy

### Game Project (C#/Unity)

**docs/agent/** would have:
- PROJECT.toon → Game overview
- ARCHITECTURE.toon → Game engine structure
- CONSTRAINTS.toon → Performance limits
- GAMEPLAY.toon → Game mechanics
- GRAPHICS.toon → Rendering pipeline
- PHYSICS.toon → Physics constraints

### ML Project (Python/TensorFlow)

**docs/agent/** would have:
- PROJECT.toon → ML pipeline overview
- ARCHITECTURE.toon → Model structure
- CONSTRAINTS.toon → Data/memory limits
- DATA.toon → Dataset description
- MODELS.toon → Model implementations
- TRAINING.toon → Training procedure

---

## ✅ Checklist: Ready for Agent Work

- [ ] CLAUDE.md created (< 500 words)
- [ ] docs/agent/README.toon exists
- [ ] docs/agent/PROJECT.toon created
- [ ] docs/agent/CONSTRAINTS.toon created
- [ ] docs/agent/QUICK_LINKS.toon created
- [ ] docs/agent/learning/ copied (project-agnostic)
- [ ] .claudesignore created (excludes docs/human/)
- [ ] All links verified (working)
- [ ] Human docs moved to docs/human/
- [ ] First agent session completes without "what is this?" questions

---

## 📖 Further Reading

- **For Mirage project**: See Mirage's `CLAUDE.md` as a live example
- **For agent self-learning**: See `docs/agent/learning/SELF-LEARNING.toon`
- **For TOON format**: See any `docs/agent/*.toon` file as reference

---

## 🚀 Quick Start Template

Want to use this immediately? Here's the minimal setup:

```bash
# 1. Create structure
mkdir -p docs/agent/learning docs/human
touch CLAUDE.md .claudesignore

# 2. Get self-learning templates
# Copy docs/agent/learning/ from Mirage project
# (These are project-agnostic)

# 3. Create basic agent docs
# Use templates in this file

# 4. Run
git add .
git commit -m "docs: initial agent documentation structure"
```

**That's it!** Now your agents can self-improve. 🎉

---

**Created by**: Claude + User (Mirage Project)  
**License**: Use freely in any project  
**Last updated**: 2026-04-16  
**Framework status**: Stable, production-ready
