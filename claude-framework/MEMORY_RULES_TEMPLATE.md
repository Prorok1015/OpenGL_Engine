# Memory Management Rules for [PROJECT_NAME]

**Purpose**: Govern what agents save to cross-session memory (~240 tokens, <0.2% context)  
**Benefit**: Accumulate knowledge between sessions without token bloat

---

## ✅ ALLOWED: What agents CAN save to memory

### 1. Explicit user rules
**Trigger**: User says "Never use X", "This is a constraint", "Remember: always do Y"

Example:
```
"Never touch database migrations without explicit approval"
→ Save: User rule about migrations
```

### 2. New architectural discoveries
**Trigger**: Agent finds undocumented pattern while reading code

Example:
```
"Layer 2 uses weak_ptr to break cycles"
→ Save: Architectural discovery about memory management pattern
```

### 3. Critical project constraints
**Trigger**: Hard limits that affect implementation decisions

Example:
```
"Max 64KB mlock buffer on Linux"
→ Save: Platform-specific constraint
```

### 4. Project status & blockers
**Trigger**: Milestones completed, epics in progress, blocking issues

Example:
```
"EPIC-15 completed (key hierarchy implemented)"
"EPIC-18 blocked on EPIC-15"
→ Save: Project status snapshot
```

### 5. User profile
**Trigger**: Learn about user's role, experience, language preferences

Example:
```
User is: Senior C++ engineer, new to React, speaks Russian
→ Save: Communication preferences and domain knowledge
```

---

## ❌ PROHIBITED: What agents MUST NOT save

### 1. Obvious facts from code
**Problem**: Readable in 5 minutes, pollutes memory

❌ Bad:
```
"Function returns nullptr on error"
"Loop iterates through array"
"Vector uses templates"
```

### 2. Temporary observations
**Problem**: Becomes stale in days

❌ Bad:
```
"This code is slow"
"TODO: refactor Layer 3"
"Looks like it might be a bug"
```

### 3. Unverified speculation
**Problem**: False information is worse than no information

❌ Bad:
```
"Probably the relay does X"
"I think developers meant..."
"Might be how it works"
```

### 4. Bug fix history
**Problem**: Belongs in git commit messages, not memory

❌ Bad:
```
"Fixed bug where X was happening"
"Changed function Y to do Z"
"Removed legacy code"
```

### 5. TODO/Backlog lists
**Problem**: Belongs in Issues/Tickets, not memory

❌ Bad:
```
"Need to implement: A, B, C"
"Should optimize: X, Y, Z"
"Future work: build V, integrate W"
```

### 6. Duplicated information
**Problem**: Already in CLAUDE.md or docs/agent/

❌ Bad:
```
"Architecture: 5 layers (api, domain, crypto, routing, transport)"
"Constraint: No raw pointers in collections"
```
(This is already in CLAUDE.md)

### 7. Process reasoning
**Problem**: Only results matter

❌ Bad:
```
"Tried approach A, didn't work"
"Then tried approach B instead"
"Finally settled on C because..."
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

- Role: Developer / Architect / etc.
- Experience: C++, Python, JavaScript, etc.
- Language: Russian / English / etc.
```

**feedback_*.md** — Rules & constraints (< 2 KB each)
```markdown
---
name: Feedback: Architecture Rules
type: feedback
---

## Rule 1: No Direct Filesystem Access
pattern: Core layer never writes to disk
why: Core is in-memory P2P router only
applies_to: mirage-core/ all layers
```

**project_*.md** — Status & discoveries (< 5 KB each)
```markdown
---
name: Project Status: Key Hierarchy
type: project
---

[DISCOVERY]
Ed25519 → X25519 key derivation implemented in identity_engine.cpp
All 6 hierarchy tasks completed
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
  Agent discovers rule
  ✅ Checks checklist above
  ✅ Saves to memory/feedback_*.md

Session 2+:
  Agent loads memory (~240 tokens)
  ✅ Knows rule immediately
  ✅ No need to ask user
  ✅ Saves ~100+ tokens per session
```

---

## 💡 Example: Correct vs Incorrect

### ❌ INCORRECT: Too generic, temporary

```markdown
# Session Notes

Agent found that:
- Code uses vectors
- Functions return nullptr
- TODO: optimize Layer 3
- Maybe relay caches connections
```

**Problems:**
- Obvious facts (vectors, nullptr)
- Temporary observation (slow)
- Speculation without verification
- No context/permanent rules

### ✅ CORRECT: Specific, permanent, contextual

```markdown
# Feedback: Architecture Rules

## Rule: Connection Pooling for Performance
pattern: Relay pools QUIC connections per peer, reuses for sequential messages
why: Reduces handshake overhead, improves throughput
applies_to: relay_session.cpp, peer management
discovered_in: Code review of relay_session.cpp:245-290
```

**Advantages:**
- Actual rule (not observation)
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
7. **Regular review** — Archive stale memories monthly
8. **User approval** — Explicit user statements are highest confidence
9. **Code verification** — Discovered rules must be verifiable in source
10. **Avoid redundancy** — Don't duplicate existing docs

---

## 📖 Integration with Self-Learning

This memory system works together with agent self-learning:

1. **Self-Learning** (docs/agent/learning/) — HOW to update documentation
2. **Memory Rules** (MEMORY_RULES.md) — WHAT to save to cross-session memory
3. **CLAUDE.md** — WHERE to link these systems

Together they create a **knowledge accumulation system** that improves each session.

---

## Next Steps

1. **Review this file** when initializing the framework
2. **Copy to MEMORY_RULES.md** in your project root (replace [PROJECT_NAME])
3. **Reference from CLAUDE.md** (already linked in CLAUDE_TEMPLATE.md)
4. **Review quarterly** to archive stale memories and update rules

---

**Version**: 1.0  
**Framework Version**: 1.0 Stable  
**Purpose**: Universal memory governance for any project using Claude agents
