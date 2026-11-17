# Quick Start: Apply This Framework to a New Project

Use this guide to set up agent documentation in any new project (< 15 minutes).

---

## Step 1: Copy Framework Files (2 minutes)

These files are universal and project-agnostic:

```bash
# Copy to your new project
cp PROJECT_MANAGEMENT_RULES.md /your/new/project/
cp CLAUDE_TEMPLATE.md /your/new/project/
cp MEMORY_RULES_TEMPLATE.md /your/new/project/
cp -r docs/agent/learning /your/new/project/docs/agent/
```

---

## Step 2: Create Project-Specific Docs (10 minutes)

### 2a. Rename CLAUDE_TEMPLATE.md to CLAUDE.md

```bash
mv CLAUDE_TEMPLATE.md CLAUDE.md
```

Then edit it:
1. Replace `[PROJECT_NAME]` with your project name
2. Replace `[Component 1/2/3]` with your actual components
3. Update the 3 critical rules for YOUR project
4. Update conventions section

**Keep it short!** Total should be < 500 words.

### 2b. Rename MEMORY_RULES_TEMPLATE.md to MEMORY_RULES.md

```bash
mv MEMORY_RULES_TEMPLATE.md MEMORY_RULES.md
```

Then edit it:
1. Replace `[PROJECT_NAME]` with your project name
2. Review the checklist (you probably don't need to change it)
3. Make sure you understand when agents should save to memory

This file governs cross-session learning (~240 tokens, <0.2% context). It's critical for agent knowledge accumulation.

### 2c. Create .claudesignore

```bash
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
*.swp

# Keep human docs separate from agent context
docs/human/

# Generated
*.log
*.tmp
EOF
```

### 2d. Create docs/agent/ structure

Create these files in `docs/agent/`:

**docs/agent/README.toon** (copy structure, fill with your details):
```toon
[DOCUMENTATION_INDEX]
format: TOON
audience: Claude agents working on [PROJECT_NAME]

[FILES]
- PROJECT.toon → Project overview
- ARCHITECTURE.toon → System design
- CONSTRAINTS.toon → Rules and limits
- QUICK_LINKS.toon → File navigation

[USAGE]
Before working on code:
1. Read PROJECT.toon for context
2. Check CONSTRAINTS.toon (STRICT!)
3. Use QUICK_LINKS.toon to find relevant files

[LEARNING]
Updating documentation:
→ See learning/SELF-LEARNING.toon
```

**docs/agent/PROJECT.toon**:
```toon
[PROJECT]
name: [YOUR_PROJECT]
type: [Type: web app, library, game, etc.]
description: [1-2 sentences]
stack: [Your tech stack]

[MODULES]
[MODULE.X]
name: [Component name]
path: [source path]
responsibility: [What it does]

[STATUS]
current_phase: [What phase are you in]
next_steps: [What's next]
```

**docs/agent/ARCHITECTURE.toon**:
```toon
[ARCHITECTURE]
pattern: [Your architecture pattern]
style: [Your approach]

[COMPONENT_1]
name: [Component name]
responsibility: [What it owns]
depends_on: [Dependencies]

[CONSTRAINTS]
rule: [Architectural rule 1]
rule: [Architectural rule 2]
```

**docs/agent/CONSTRAINTS.toon**:
```toon
[CONSTRAINTS]
severity: CRITICAL (rules for all code)

[CONSTRAINT_1]
rule: [Your first critical rule]
why: [Reason]
applies_to: [Who should follow it]

[CONSTRAINT_2]
rule: [Your second critical rule]
...
```

**docs/agent/QUICK_LINKS.toon**:
```toon
[QUICK_LINKS]
purpose: Fast file lookup

[CODE_STRUCTURE]
Component 1 → [path/to/component1]
Component 2 → [path/to/component2]

[DOCUMENTATION]
Project overview → PROJECT.toon
Architecture → ARCHITECTURE.toon
Rules → CONSTRAINTS.toon
```

---

## Step 3: Organize Existing Documentation (3 minutes)

```bash
# Move existing docs to human/
mkdir -p docs/human
mv docs/your-old-docs docs/human/

# Or if you have README files
mv *.md docs/human/
```

---

## Step 4: Initialize Git (optional but recommended)

```bash
git add CLAUDE.md MEMORY_RULES.md .claudesignore docs/
git commit -m "docs: initial agent documentation structure

Framework: Universal Project Management Framework for Claude AI
Structure: docs/agent (compact) + docs/human (full)
Self-learning: docs/agent/learning/
Memory: MEMORY_RULES.md governs cross-session knowledge"
```

---

## Step 5: Test with Claude

### Option A: Pass Framework to Claude

Copy/paste this in Claude chat:

```
I've set up agent documentation for my [PROJECT_TYPE] project.

Project structure:
- CLAUDE.md: [paste content]
- docs/agent/PROJECT.toon: [paste content]
- docs/agent/ARCHITECTURE.toon: [paste content]
- docs/agent/CONSTRAINTS.toon: [paste content]

Framework reference: [paste PROJECT_MANAGEMENT_RULES.md if needed]

Help me review this setup and refine the documentation.
Then start working on [YOUR_FIRST_TASK].
```

### Option B: Just Start Using It

```
I have agent documentation set up at:
- docs/agent/README.toon (navigation)
- docs/agent/PROJECT.toon (overview)
- docs/agent/CONSTRAINTS.toon (rules)
- etc.

Can you read these and help me with [YOUR_TASK]?
```

---

## 🎯 Before/After Comparison

### Before This Framework
```
Agent starts work:
  ❓ What is this project?
  ❓ What are the constraints?
  ❓ Where are the files?
  → Agent asks user 5+ questions before starting
  → Token budget wasted on explanations
```

### After This Framework
```
Agent starts work:
  ✅ Reads CLAUDE.md (essential rules)
  ✅ Checks docs/agent/PROJECT.toon (quick context)
  ✅ Uses docs/agent/QUICK_LINKS.toon (find files)
  → Agent starts immediately
  → Only asks clarifying questions about work
```

---

## 📋 Checklist: Ready for Agents?

- [ ] CLAUDE.md created and customized for your project
- [ ] docs/agent/README.toon exists
- [ ] docs/agent/PROJECT.toon created
- [ ] docs/agent/ARCHITECTURE.toon created
- [ ] docs/agent/CONSTRAINTS.toon created
- [ ] docs/agent/QUICK_LINKS.toon created
- [ ] docs/agent/learning/ directory copied (has 7 .toon files)
- [ ] .claudesignore created
- [ ] All links verified
- [ ] Existing docs moved to docs/human/ (if any)

**All checked?** → Ready to work with agents! 🚀

---

## 📊 Token Budget Check

```
Your documentation uses:
- CLAUDE.md: ~150 tokens (loaded always)
- docs/agent/*.toon: ~2,000 tokens (loaded by reference)
- docs/agent/learning/*: ~1,600 tokens (loaded on need)

Total agent context: ~3,750 tokens (from ~15,000+ before)
```

---

## 🔄 Next Sessions

After first session, agents will:
1. Read CLAUDE.md (quick reminder)
2. Reference docs/agent/*.toon (load what needed)
3. **Improve docs** when discovering rules (self-learning)
4. Update docs/agent/learning files if needed
5. Make git commits with `[docs]` prefix

Result: Documentation gets better each session! 📈

---

## 🆘 Troubleshooting

### "Agent doesn't understand my project"
→ Check CLAUDE.md and docs/agent/PROJECT.toon are clear  
→ Run agent again, let it ask clarifying questions  
→ Update docs based on answers

### "Links in docs are broken"
→ Use relative paths: `docs/agent/FILE.toon`  
→ Check file exists in correct location  
→ Verify file names match exactly

### "Documentation is too long"
→ Move details to separate .toon files  
→ Create new file in docs/agent/ for the topic  
→ Link to it from other files

### "Agent keeps asking same questions"
→ Document answers in docs/agent/.toon files  
→ Make sure QUICK_LINKS.toon covers common searches  
→ Add examples to EXAMPLES.toon

---

## 🎓 Example: Python Web Project

Your structure might look like:

```
my-web-app/
├── CLAUDE.md
│   - Django project
│   - Rules: no raw SQL, use ORM
│   - Critical: migrations must be reversible
│
├── .claudesignore
│   - __pycache__/, venv/, node_modules/
│   - docs/human/
│
└── docs/
    ├── agent/
    │   ├── PROJECT.toon (Django + React stack)
    │   ├── ARCHITECTURE.toon (MVC pattern, API structure)
    │   ├── CONSTRAINTS.toon (ORM rules, migration rules)
    │   ├── API.toon (DRF endpoints)
    │   ├── DATABASE.toon (Model descriptions)
    │   ├── TESTING.toon (Test strategy)
    │   └── learning/ (7 TOON files, reused)
    │
    └── human/ (old README, design docs, etc.)
```

---

## ✅ You're Done!

Your project now has:
- ✅ Modular agent documentation
- ✅ Self-learning mechanism
- ✅ Clear separation (human vs agent docs)
- ✅ Token-efficient context
- ✅ Growing knowledge base (each session improves it)

**Time to start working!** 🚀

---

**Questions?** See `PROJECT_MANAGEMENT_RULES.md` for detailed reference.
