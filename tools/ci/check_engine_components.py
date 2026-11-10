#!/usr/bin/env python3
"""Fails when the engine creates a component on a world whose type is not in the engine component catalog (#620).

    check_engine_components.py [<engine source root>]   (default: core/engine)
    check_engine_components.py --self-test

A component the catalog does not know cannot be saved by a hot reload snapshot: the reload refuses the world
(scn::unregistered_storages, HOT_RELOAD_PLAY.md section 4.3). tests/scn_engine_component_catalog_tests.cpp checks what
the desc spawners make; this checks the sources for the rest (systems, the snapshot, prefab instancing).

Every emplace<X>, emplace_or_replace<X> and get_or_emplace<X> on a registry in the engine's sources (calls through
ctx() are context values, not components) must name a type registered with add_engine_component / add_engine_derived
(any file of the engine) or be allowed in that file by ALLOWED below, with its reason. The match is by the last
identifier of the type (scn::local_transform and local_transform are one).

The check reads one line at a time, so it does not see:
  - a call whose template argument is on the next line (`reg.emplace<` then the type);
  - other ways to make a component: insert<X>, storage<X>().emplace, patch;
  - a type alias, or a type reached through a generic template of another name;
  - ctx() on the line before the call or kept in a variable (`auto& ctx = reg.ctx(); ctx.emplace<X>()` is reported as a
    component: allow it with the reason);
  - calls inside /* */ comments (// comments are skipped).
The catalog test (tests/scn_engine_component_catalog_tests.cpp) and the hot reload's refusal of a world holding an
unknown storage (scn::unregistered_storages) cover what this misses.
"""
import re
import sys
from pathlib import Path

# (file name, type) created on a world that is not a catalog entry, and why. A stale entry (nothing of that name is
# created in that file any more) fails.
ALLOWED = {
    ('scn_component_catalog.h', 'T'): 'component_entry::read, the generic reader of every registered type',
}

SUFFIXES = ('.cpp', '.h', '.hpp')
CREATE = re.compile(
    r'(?:\.|->)\s*(?:template\s+)?(?P<fn>emplace_or_replace|get_or_emplace|emplace)\s*<\s*(?P<type>[A-Za-z_][\w:]*)')
# What precedes the "." or "->" of a creation that is a context value.
CONTEXT_CALL = re.compile(r'ctx\(\)\s*$')
REGISTER = re.compile(r'\badd_engine_\w+\s*<\s*(?P<type>[A-Za-z_][\w:]*)\s*>')


def last_name(type_name):
    return type_name.split('::')[-1]


def scan(files):
    """files: (path, text) pairs. Returns (registered names, [(path, line, type)] of creations)."""
    registered = set()
    created = []
    for path, text in files:
        for match in REGISTER.finditer(text):
            registered.add(last_name(match.group('type')))
        for number, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith('//'):
                continue
            for match in CREATE.finditer(line):
                if CONTEXT_CALL.search(line[:match.start()]):
                    continue
                created.append((path, number, match.group('type')))
    return registered, created


def problems(files, allowed=ALLOWED):
    registered, created = scan(files)
    errors = []
    seen = set()
    for path, number, type_name in created:
        name = last_name(type_name)
        key = (Path(path).name, name)
        seen.add(key)
        if name in registered or key in allowed:
            continue
        errors.append(f"{path}:{number}: '{type_name}' is created on a world but is not in the engine component catalog: "
                      f"register it in scn::register_engine_components (add_engine_component, or add_engine_derived when "
                      f"it is recomputed and not saved), or add it to ALLOWED in tools/ci/check_engine_components.py "
                      f"with the reason")
    for file_name, name in sorted(set(allowed) - seen):
        errors.append(f"tools/ci/check_engine_components.py: ALLOWED names '{name}' in {file_name}, which no longer "
                      f"creates it: remove it")
    return errors


def read_tree(root):
    for path in sorted(Path(root).rglob('*')):
        if path.suffix in SUFFIXES and path.is_file():
            yield str(path), path.read_text(encoding='utf-8', errors='replace')


def expect(condition, what):
    # Not assert: python -O would skip every check.
    if not condition:
        raise SystemExit(f'check_engine_components: self-test failed: {what}')


def self_test():
    registry = ('reg.cpp', 'add_engine_component<local_transform>(catalog, "scn.local_transform");\n'
                           'add_engine_derived<scn::world_transform>(catalog, "scn.world_transform");\n')
    good = ('dir/scn_component_catalog.h', 'reg.emplace<scn::local_transform>(e);\n'
                                           'reg.emplace_or_replace<world_transform>(e, m);\n'
                                           'registry->template get_or_emplace<scn::local_transform>(e);\n'
                                           'reg.ctx().emplace<scn::node_index>();\n'
                                           'reg.ctx().template emplace<ecs::world_bus>();\n'
                                           '// reg.emplace<commented_out>(e);\n'
                                           'reg.emplace_or_replace<T>(e, std::move(value));\n')
    errors = problems([registry, good])
    expect(errors == [], f'a clean tree reports {errors}')

    bad = ('b.cpp', 'void f() {\n    reg.emplace_or_replace<scn::mouse_state>(e);\n}\n')
    errors = problems([registry, good, bad])
    expect(len(errors) == 1 and errors[0].startswith("b.cpp:2: 'scn::mouse_state'"), f'an unregistered type gives {errors}')

    elsewhere = ('other.cpp', 'reg.emplace<T>(e);\n')
    errors = problems([registry, good, elsewhere])
    expect(len(errors) == 1 and errors[0].startswith("other.cpp:1: 'T'"), f'T outside its allowed file gives {errors}')

    stale = problems([registry, good], allowed={('scn_component_catalog.h', 'T'): 'generic', ('x.cpp', 'gone'): 'old'})
    expect(len(stale) == 1 and "'gone' in x.cpp" in stale[0], f'a stale allowance gives {stale}')

    _, created = scan([('c.cpp', 'reg.emplace<a>(e); reg.emplace<b::c>(e);\n')])
    expect([entry[2] for entry in created] == ['a', 'b::c'], f'two calls on a line give {created}')
    print('check_engine_components: self-test passed')


def main(argv):
    if argv[1:] == ['--self-test']:
        self_test()
        return 0
    root = argv[1] if len(argv) > 1 else 'core/engine'
    if not Path(root).is_dir():
        print(f'check_engine_components: {root} is not a directory (run from the repository root or pass the engine '
              f'source root)')
        return 2
    errors = problems(list(read_tree(root)))
    for error in errors:
        print(error)
    if errors:
        print(f'check_engine_components: {len(errors)} problem(s)')
        return 1
    print('check_engine_components: every component the engine creates is in its catalog')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
