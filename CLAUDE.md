<!-- framework-meta: framework=claude-framework class=parameterized target=CLAUDE.md version=1.3.0 -->
<!-- The line above is a framework marker. Do NOT remove it. It tells PATCH_PROMPT which template produced this file. -->

# CLAUDE.md — OpenGL Engine & Editor

## 🎯 Project

**OpenGL Engine** — C++20 3D graphics engine with real-time scene editor, multi-world ECS, data-driven rendering pipeline, and asset management system.

Key tech: C++20, OpenGL 4.5+, ImGui, CMake, EnTT ECS, Assimp, Boost 1.84.0

## 📂 Structure

- **Core** — ECS, resource system, descriptors, memory allocators, configuration
- **Engine** — Scene management, render pipeline, animation, input systems
- **Editor** — ImGui-based scene editor with prefab and shader support

📚 **Documentation:**
- 👉 [Full project structure](PROJECT_STRUCTURE.md)
- 🤖 [Agent documentation index](docs/agent/README.toon) — Compact, modular
- 🏗️ [ARCHITECTURE.toon](docs/agent/ARCHITECTURE.toon) — System design & layers
- 🎨 [CODE_CONVENTIONS.toon](docs/agent/CODE_CONVENTIONS.toon) — Naming & style
- ⚡ [CONSTRAINTS.toon](docs/agent/CONSTRAINTS.toon) — Hard rules & limits
- 🔗 [QUICK_LINKS.toon](docs/agent/QUICK_LINKS.toon) — File navigation
- 📋 [docs/epics/INDEX.md](docs/epics/INDEX.md) — Task management (epics and their statuses)

## ⚠️ Critical Rules (STRICT!)

### No iostream, no std::cout/cerr
- **Use:** `#include <snk/common/logger/engine_log.h>` → `egLOG`/`egLOG_INFO`, `egLOG_WARN`, `egLOG_ERROR` (also `egLOG_TRACE`/`egLOG_DEBUG`)
- **Signature:** first arg is a category string, then `std::format` args: `egLOG_WARN("resource", "not found: {}", tag)`
- **Why:** Formatted, filterable per-category logging routed through spdlog

### Never store frame-allocator pointers across frames
- **What:** `std::pmr::vector<T>(ds::frame_allocator())` invalidated at frame reset
- **Why:** Frame allocator memory reclaimed at `ds::frame_allocator_reset()` (frame start)
- **Fix:** Always transfer data or use `std::make_shared<T>()` for persistence

### Always reserve() before pushing frame-temp data
- **Pattern:** `std::pmr::vector<T> v(ds::frame_allocator()); v.reserve(est); ...`
- **Why:** Avoids grow-and-copy within bump allocator (wastes memory)

### C++20 only, snake_case, tabs (size 4)
- **Headers:** `#pragma once`, include order: module → stdlib → third-party → project
- **Braces:** functions on new line, control flow same line
- **Assertions:** `ASSERT_MSG`/`ASSERT_FAIL` from `<snk/common/engine_assert.h>`, never `assert()`

### ImGui: `End()` is unconditional
- **Pattern:** `if (ImGui::Begin("Name")) { ...draw... } ImGui::End();` (same for `BeginChild`/`EndChild`)
- **Exception:** `BeginPopup*`/`BeginMenu`/`BeginTable`/etc. → call `End*` only if `Begin*` returned true
- **Ref:** `Editor/code/editor_system/panels/edt_panel_base.cpp`

### Never bypass resource cache
- **Use:** get `res::resource_system&` by injection (constructor parameter / `app_data_storage` in composition code), then `.require<T>(res::tag{"res://path"})` (or `require_sync<T>` for blocking) → returns `res_handle<T>`
- **No global accessor:** `res::get_system()` (and `rnd::`/`gui::get_system()`) no longer exist — never reintroduce a global/singleton; inject `res::resource_system&`
- **Never:** hardcode filenames, new/load directly from disk

## 🗣️ Conventions

- **Language:** English (code/comments), Russian (docs/epics/)
- **Namespaces:** 2-4 letter abbrev (`ds`, `ecs`, `scn`, `rnd`, `res`, `edt`, etc.)
- **Files:** `prefix_name.hpp/.cpp` (e.g. `scn_context_set.cpp`, `res_system.h`)
- **Private members:** `m_` prefix (`m_worlds`)
- **Components:** POD structs, optional `_component` suffix
- **Systems:** Inherit `ecs::system_interface` (auto-registered)

## 🎫 Task Tracking — GitHub Issues (primary)

All work is tracked in [GitHub Issues](https://github.com/Prorok1015/OpenGL_Engine/issues) — status and progress live there, not in markdown.

- **Epic** = issue with label `epic`; its stories are native **sub-issues** (one issue per user story `US-NN-X`: description, files, AC checklist, link to the epic).
- **Bug** = issue with label `bug`, attached to its epic when there is one.
- **Labels:** `epic` `story` `bug` `tech-debt` `docs` `ci` · themes `editor` `render` `ecs` `resource` `infrastructure` `desc` `chimera` · priority `P0`–`P2`.
- **Milestones:** Фаза 0 — этап C / этап D, M1–M6 (Chimera roadmap), Этап Б (ТЗ).
- **Design docs stay in the repo:** `docs/epics/**/EPIC-NN.md`, `docs/roadmap/*.md` hold motivation and architecture; each links to its issue (`**Issue:** #N`) and each epic issue links back.
- **Before starting work:** find the issue (or create one for non-trivial work); read the linked design doc.
- **Commits:** reference the issue — `refs #N` while in progress, `closes #N` in the commit that finishes it; close the issue (and tick AC) when done. Bugs found mid-task → new `bug` issue, not a silent fix in an unrelated commit.
- **Session handoff:** [`docs/agent/handoff/PHASE0-STATUS.md`](docs/agent/handoff/PHASE0-STATUS.md) — current phase, what's done, key issues and agreements.

## 📚 Backlog (session scratch)

The agent keeps a lightweight backlog at `docs/agent/backlog/`. Use it when the user says "do this later", when workflow phases produce out-of-scope follow-ups, or when side-quests should be captured without breaking the current task.

👉 **Start here:** [`BACKLOG-RULES.toon`](docs/agent/backlog/BACKLOG-RULES.toon)

Then use these guides:
- 🧩 [`SCHEMA.toon`](docs/agent/backlog/SCHEMA.toon) — Item format (7-line INDEX entry + detail file)
- 🔁 [`LIFECYCLE.toon`](docs/agent/backlog/LIFECYCLE.toon) — proposed → accepted → in-progress → done
- 🛠️ [`OPERATIONS.toon`](docs/agent/backlog/OPERATIONS.toon) — add / list / show / update / activate / archive
- 🔄 [`SYNC.toon`](docs/agent/backlog/SYNC.toon) — Manual sync with external trackers (none configured)

Data: [`INDEX.toon`](docs/agent/backlog/INDEX.toon) (flat index) + `items/<id>.toon` (details, on demand).

Note: the local backlog is only for short-lived follow-ups within a session; anything that outlives the session goes to GitHub Issues (`sync_target: github`, see `SYNC.toon` → `[PROJECT_POLICY]`).

## 🗺️ Development Workflow (MANDATORY)

For features / refactors / non-trivial bugfixes — do NOT start coding immediately. Run: **DISCOVER → PLAN → CONFIRM → IMPLEMENT → VERIFY → REPORT**.

👉 **Start here:** [`WORKFLOW.toon`](docs/agent/workflow/WORKFLOW.toon)

- 📋 [`PHASES.toon`](docs/agent/workflow/PHASES.toon) — 6 phases and exit criteria
- ✨ [`FEATURE.toon`](docs/agent/workflow/FEATURE.toon) — New-feature rules
- 🔧 [`REFACTOR.toon`](docs/agent/workflow/REFACTOR.toon) — Behavior-preserving change rules
- 🐛 [`BUGFIX.toon`](docs/agent/workflow/BUGFIX.toon) — Reproduce-first bug-fix rules
- 🚦 [`GATES.toon`](docs/agent/workflow/GATES.toon) — Where to STOP and ask
- 💾 [`STATE_TRACKING.toon`](docs/agent/workflow/STATE_TRACKING.toon) — TodoWrite + `.claude/workplan/*.md`
- ⏩ [`ESCAPE_HATCH.toon`](docs/agent/workflow/ESCAPE_HATCH.toon) — When workflow may be skipped
- ✅ [`CI_PR.toon`](docs/agent/workflow/CI_PR.toon) — Deliver via draft PR, CI is the verification; agents never build locally

Rules of thumb:
- No production code before an approved plan
- "Sounds good" / "maybe" are NOT approval — only explicit "yes / proceed"
- At start of every turn, check `.claude/workplan/` and resume from recorded phase
- Code is verified by CI on a draft PR, not locally — agents do not build or run tests themselves (CI is free, run PRs in parallel)

## 🤝 Delegation to Subagents

Before any non-trivial task, decide whether to do it inline or delegate. Delegation saves main-context tokens and routes mechanical work to cheaper models (e.g. Haiku).

👉 **Start here:** [`DELEGATION.toon`](docs/agent/delegation/DELEGATION.toon)

- 🎯 [`TRIGGERS.toon`](docs/agent/delegation/TRIGGERS.toon) — When delegation is required
- 🧭 [`ROUTING.toon`](docs/agent/delegation/ROUTING.toon) — Which model for which task
- 📐 [`PATTERNS.toon`](docs/agent/delegation/PATTERNS.toon) — Copy-paste `Agent()` invocations
- 🚫 [`ANTI_PATTERNS.toon`](docs/agent/delegation/ANTI_PATTERNS.toon) — When NOT to delegate
- ⏱️ [`BUDGETS.toon`](docs/agent/delegation/BUDGETS.toon) — Token thresholds and length caps

## 🤖 Self-Learning & Documentation Updates

When you discover rules or architectural patterns:

👉 **Start here:** [`SELF-LEARNING.toon`](docs/agent/learning/SELF-LEARNING.toon)

- 📖 [`WHEN_TO_UPDATE.toon`](docs/agent/learning/WHEN_TO_UPDATE.toon) — Criteria
- ❌ [`WHEN_NOT_TO_UPDATE.toon`](docs/agent/learning/WHEN_NOT_TO_UPDATE.toon) — Veto list
- 🎯 [`FILES_REFERENCE.toon`](docs/agent/learning/FILES_REFERENCE.toon) — Which file
- 📝 [`HOW_TO_UPDATE.toon`](docs/agent/learning/HOW_TO_UPDATE.toon) — Process
- 💡 [`EXAMPLES.toon`](docs/agent/learning/EXAMPLES.toon) — Real examples
- 🛡️ [`GUARDRAILS.toon`](docs/agent/learning/GUARDRAILS.toon) — Safety checks

Commit format: `git commit -m "docs: [type] brief desc\n\nSource: ...\nFile: ...\n\nCo-Authored-By: ..."`

## 📋 Current Status

- Status and progress: GitHub Issues + milestones (open/closed); current phase summary in `docs/agent/handoff/PHASE0-STATUS.md`

👉 **All tasks:** [GitHub Issues](https://github.com/Prorok1015/OpenGL_Engine/issues) · [milestones](https://github.com/Prorok1015/OpenGL_Engine/milestones) · design index [docs/epics/INDEX.md](docs/epics/INDEX.md)

## 🚀 Build & Test

```bash
cmake --preset dev-debug                      # Ninja, build/dev-debug; ccache/sccache + PCH on by default
cmake --build build/dev-debug --target editor unit_tests
build/dev-debug/unit_tests [--run_test=SuiteName]
build/dev-debug/Editor/editor --config Editor/config/editor.cfg
# presets: dev-debug dev-release release no-assimp, and ci-* (the exact CI configurations, e.g. ci-linux-asan)
# without presets: mkdir build && cd build && cmake .. && make -j$(nproc)   (cache/PCH options still apply)
```

Build speed (CMake options, so local/IDE/CI behave the same):
- `SNK_COMPILER_CACHE=AUTO|ON|OFF` — launcher via ccache (sccache on Windows); a user-set `CMAKE_<LANG>_COMPILER_LAUNCHER` wins
- `SNK_PCH=AUTO|ON|OFF` — one shared PCH (`cmake/pch/snk_pch.h`) for project targets; AUTO = ON except MSVC with a cache. Missing include that PCH hides? Build with `-DSNK_PCH=OFF` (CI job `ci-linux-clang-debug` does)
- `SNK_WARNINGS_AS_ERRORS=ON|OFF` — `-Werror` (GCC/Clang) / `/WX` (MSVC) for project targets only, never for `lib3dparty`/Boost/Assimp; ON in every preset and in CI. New compiler with fresh warnings? Build locally with `-DSNK_WARNINGS_AS_ERRORS=OFF`, then fix them
- `SNK_ASSIMP_FORMATS=MINIMAL|ALL` — MINIMAL (default) builds only the glTF/glb, OBJ, FBX importers and no exporters; `release` preset and `release.yml` use ALL. Need another format locally? `-DSNK_ASSIMP_FORMATS=ALL`

---

## 📊 Context Optimization

This CLAUDE.md is deliberately compact. For details:
- **Agent documentation** (TOON): `docs/agent/` (load on demand)
- **Memory rules** (cross-session): `MEMORY_RULES.md`
- **Framework state**: `.claude-framework` (do not edit manually)
- **Framework version**: 1.3.0

The key: Load only what you need, when you need it. 🎯
