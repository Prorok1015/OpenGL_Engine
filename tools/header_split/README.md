# header_split

Include-rewrite tool for the physical `include/snk/<name>/` header split (issue #109,
design: `docs/roadmap/design/HEADER_SPLIT.md`). Python 3, stdlib only. Nothing in the
repo is moved by this change; `DRY_RUN.md` has the numbers for each PR group.

- `components.json` - per component: `root`, `dest` (`snk/<name>`), explicit `public` and
  `internal` header lists (relative to `root`), and the `pr_groups` (PR-1..PR-4).
  Subfolders under `root` are kept: `scene/level/scn_level.h` -> `snk/scene/level/scn_level.h`.
- `split.py` - the tool. Sources are read and written as bytes, so CRLF/LF and every byte
  outside an include path stay exactly as they are. `lib3dparty/`, `build*/` and `.git` are skipped.
- `test_split.py` - `python3 -m unittest` (run from this directory; needs `git`).

## Workflow for a PR-1..PR-4 agent

```bash
cd tools/header_split
python3 split.py check                    # baseline invariants (a), (b), (c)
python3 split.py plan PR-1                # or component names: plan application window
#   review: git mv list, every "file:line: old -> new", ambiguities, warnings
python3 split.py apply PR-1               # git mv + rewrite includes, then self-check (c)
python3 split.py check
```

Then do the CMake part from PR-0 (`snk_component_headers(<tgt>)` etc.: public
`$<BUILD_INTERFACE:include>`, component root PRIVATE) and build/test as the design requires.
Commit the `git mv` + rewrite as one commit so the rename stays visible, and add its SHA
to `.git-blame-ignore-revs` in PR-5.

## Rules the tool enforces

- An include is resolved by basename plus optional subpath (`"x.h"`, `"sub/x.h"`, `<x.h>`).
  For quoted includes a header next to the including file wins, like the compiler does.
  Any other multi-match is an ambiguity: `plan` lists it, `apply` refuses and changes nothing.
- Only public headers of the selected components move; only includes of those headers change.
  Internal headers stay next to the sources. `apply` is idempotent.
- `check`: (a) no basename collisions among project headers, (b) no public header includes an
  internal one (exit code 1 until the classification in `components.json` or the headers are fixed),
  (c) no quoted/flat include of an already moved header remains. It also reports headers inside a
  component root that are in neither list (add them to `components.json`).
- `plan` warns when an internal header of the selected components is included from outside its
  component: that consumer needs PRIVATE include access in CMake, or the header must be promoted.

`report` regenerates `DRY_RUN.md` content (`python3 split.py report > DRY_RUN.md`) on a tree that
has not been migrated yet.
