<!-- framework-meta: framework=claude-framework class=parameterized target=MEMORY_RULES.md version=1.0.0 -->
<!-- The line above is a framework marker. Do NOT remove it. It tells PATCH_PROMPT which template produced this file. -->

# Memory Management Rules — OpenGL Engine

**Purpose**: Govern what agents save to cross-session memory (~240 tokens, <0.2% context)
**Benefit**: Accumulate knowledge between sessions without token bloat
**Relation to built-in memory**: This file is a *project-level policy layer* on top of the agent harness's built-in auto-memory system. Built-in rules (user/feedback/project/reference types) still apply; this file adds project-specific guidance.

---

## ✅ ALLOWED: What agents CAN save to memory

### 1. Explicit user rules
**Trigger**: User says "Never use X", "This is a constraint", "Remember: always do Y"

Example:
```
"Never use std::cout, always use engine_log.h"
→ Save: Critical logging rule
```

### 2. New architectural discoveries
**Trigger**: Agent finds undocumented pattern while reading code

Example:
```
"Frame allocator resets at frame start, invalidating all frame-temp pointers"
→ Save: Memory management pattern discovery
```

### 3. Critical project constraints
**Trigger**: Hard limits that affect implementation decisions

Example:
```
"ECS supports max 8 independent worlds via world_salt + bind_res"
"Frame allocator uses bump allocation (deallocate() is no-op)"
→ Save: Architecture-level constraints
```

### 4. Project status & blockers
**Trigger**: Milestones completed, epics in progress, blocking issues

Example:
```
"EPIC-25 (Prefab Editor) in progress, depends on EPIC-12,13,14"
"EPIC-33 (Skinning) blocks EPIC-48 (GPU batching)"
→ Save: Epic dependency and status
```

### 5. User profile
**Trigger**: Learn about user's role, experience, language preferences

Example:
```
User is: Active C++ developer, working on EPIC-25
Preference: Russian for epic docs, English for code comments
→ Save: Communication and work preferences
```

---

## ❌ PROHIBITED: What agents MUST NOT save

### 1. Obvious facts from code
**Problem**: Readable in 5 minutes, pollutes memory

❌ Bad:
```
"ImGui::Begin() is used for panels"
"Components inherit from POD structs"
"Systems inherit ecs::system_interface"
```

### 2. Temporary observations
**Problem**: Becomes stale in days

❌ Bad:
```
"This code is slow, needs optimization"
"TODO: refactor render pipeline"
"Looks like performance issue"
```

### 3. Unverified speculation
**Problem**: False information is worse than no information

❌ Bad:
```
"Probably resource cache uses LRU"
"I think ECS systems are thread-safe"
"Might be how descriptors work"
```

### 4. Bug fix history
**Problem**: Belongs in git commit messages, not memory

❌ Bad:
```
"Fixed frame allocator leak in render_packet.cpp"
"Changed ImGui panel layout"
"Removed old renderer code"
```

### 5. TODO/Backlog lists
**Problem**: Belongs in docs/epics/, not memory

❌ Bad:
```
"Need to implement: Material Editor, Shader Desc"
"Should optimize: Render extraction, system scheduling"
"Future: workspace system, profiler integration"
```

### 6. Duplicated information
**Problem**: Already in CLAUDE.md, CONSTRAINTS.toon, or ARCHITECTURE.toon

❌ Bad:
```
"Architecture: Core→Engine→Editor 4-layer pyramid"
"Constraint: Never store frame-allocator pointers across frames"
```
(This is already in project documentation)

### 7. Process reasoning
**Problem**: Only results matter

❌ Bad:
```
"Tried approach A, didn't work"
"Then tried approach B instead"
"Finally settled on C because it's cleaner"
```

---

## 📋 Memory File Structure

**Location**: `~/.claude/projects/[PROJECT_ID]/memory/`

### Files created automatically:

**MEMORY.md** — Index (< 1 KB)
```markdown
- [User Profile](user_profile.md) — role, experience
- [Feedback: Code Style](feedback_code_style.md) — coding rules
- [Project Status](project_status.md) — epic completion, blockers
```

**user_profile.md** — User context (< 1 KB)
```markdown
---
name: User Profile
type: user
---

- Role: C++ Engine Developer
- Current Work: EPIC-25 Prefab Editor
- Language: Russian (docs/epics/), English (code)
```

**feedback_*.md** — Rules & constraints (< 2 KB each)
```markdown
---
name: Feedback: Memory System Rules
type: feedback
---

## Rule: Frame Allocator Lifespan
pattern: Never store std::pmr::vector<T>(ds::frame_allocator()) across frames
why: Frame allocator memory invalidated at ds::frame_allocator_reset()
applies_to: All hot paths, render pipeline, system updates
```

**project_*.md** — Status & discoveries (< 5 KB each)
```markdown
---
name: Project Status: Epic Progress
type: project
---

[IN_PROGRESS]
- EPIC-25 Prefab Editor: Panel UI done, live preview WIP
- EPIC-33 Skinning: Descriptor-driven arch done, GPU skinning TBD

[DEPENDENCIES]
- EPIC-25 blocks: EPIC-26 (Shader Desc) dependency
- EPIC-48 blocked by: EPIC-45 (Render Optimization)
```

---

## ✅ Checklist before saving to memory

Before adding anything to memory, answer:

- [ ] Is this information **critical** for project work?
- [ ] Will this be **relevant in 1 month**? (not just today)
- [ ] Is this **NOT obvious** from reading the code?
- [ ] Does this **NOT duplicate** CLAUDE.md or docs/agent/?
- [ ] Is this a **fact**, not speculation?
- [ ] Is this a **permanent rule**, not a temporary observation?

**All YES?** → Save it  
**Any NO?** → Skip it

---

## 📊 Memory Size Control

**Recommended limits:**
```
MEMORY.md:        < 1 KB
user_profile.md:  < 1 KB
feedback_*.md:    < 2 KB each (total ~6 KB)
project_*.md:     < 5 KB each (total ~10 KB)
─────────────────────────────
TOTAL:            < 20-25 KB
```

**If memory exceeds 30 KB:**
1. Check for duplicates (remove them)
2. Archive old memories (rename to `_archive_*.md`)
3. Delete obvious facts (re-readable from code)

---

## 🎯 Memory Lifecycle

```
Session 1:
  Agent discovers rule (e.g., frame allocator pattern)
  ✅ Checks checklist above
  ✅ Saves to memory/project_memory_allocators.md

Session 2+:
  Agent loads memory (~240 tokens)
  ✅ Knows pattern immediately
  ✅ No need to re-discover
  ✅ Saves ~100+ tokens per session
```

---

## 💡 Example: Correct vs Incorrect

### ❌ INCORRECT: Too generic, temporary

```markdown
# Session Notes

Agent found that:
- ImGui panels use Begin()/End()
- ECS systems are registered automatically
- TODO: optimize render extraction
- Maybe allocators use bump allocation
```

**Problems:**
- Obvious facts (ImGui patterns, ECS)
- Temporary observation (optimization)
- Speculation without verification
- No context/permanent rules

### ✅ CORRECT: Specific, permanent, contextual

```markdown
# Project Discovery: Memory Allocation Patterns

## Rule: Frame Allocator Constraint
pattern: All frame-temp containers use ds::frame_allocator()
why: Bump allocator resets every frame, zero heap overhead in hot paths
applies_to: Render packet generation, system updates, scene extraction
found_in: ARCHITECTURE.toon, CONSTRAINTS.toon [CONSTRAINT_1-3]
discovered_via: Code review of render_packet.cpp and mem_allocator.h

## Rule: Reserve Before Push
pattern: vec.reserve(estimated_count) before vec.push_back()
why: Avoids grow-and-copy within bump allocator (wastes memory)
example: std::pmr::vector<rnd::render_packet> packets(ds::frame_allocator());
         packets.reserve(estimated_count);
```

**Advantages:**
- Actual rules (not observations)
- Has context (why, where applies)
- Verifiable in code
- Reusable across sessions

---

## 🛡️ Safety Guardrails

1. **No speculation** — Only save facts verified in code
2. **No duplication** — Check CLAUDE.md and docs/agent/ first
3. **No temporary data** — Ask "Will this matter in 1 month?"
4. **No process logs** — Save results, not the journey
5. **Size control** — Keep total < 25 KB
6. **Clear context** — Include **why** and **where** for rules
7. **Regular review** — Archive stale memories every 2-3 months
8. **User approval** — Explicit user statements are highest confidence
9. **Code verification** — Discovered rules must be verifiable in source
10. **Avoid redundancy** — Don't duplicate existing docs/epics

---

## 📖 Integration with Self-Learning

This memory system works together with agent self-learning:

1. **Self-Learning** (docs/agent/learning/) — HOW to update documentation
2. **Memory Rules** (MEMORY_RULES.md) — WHAT to save to cross-session memory
3. **CLAUDE.md** — WHERE to link these systems

Together they create a **knowledge accumulation system** that improves each session.

---

## Next Steps

1. **Review this file** when working on new features
2. **Reference from CLAUDE.md** (already linked)
3. **Save discoveries** to memory following this guide
4. **Review monthly** to archive stale memories and update rules

---

**Version**: 1.0  
**Framework Version**: 1.0 Stable  
**Purpose**: Memory governance for OpenGL Engine project with Claude agents
