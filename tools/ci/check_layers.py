#!/usr/bin/env python3
"""Fails when a lower layer of the engine reaches into a higher one (#1161).

    check_layers.py [--repo <root>]   check the sources
    check_layers.py --self-test

Layers, low to high: core/core < core/engine < core/model_import < everything else (Editor, tools, apps, tests). Code may
use its own layer and the ones below it, never one above:
  - an `#include <snk/<name>/...>` in a file under core/ names a header whose component (the folder holding
    include/snk/<name> or include_base/snk/<name>) is in a higher layer;
  - a CMakeLists.txt under core/ names, in target_link_libraries, a snk:: target defined in a higher layer, or puts a
    directory of a higher layer into target_include_directories.
The layers above core/model_import are not checked against each other.

It also fails when a public header of a target under core/ includes a component its target does not reach through
PUBLIC/INTERFACE links (a hidden dependency: it builds only through the aggregates core_api/engine_api).
"""
import re
import sys
from pathlib import Path

LAYERS = ('core/core/', 'core/engine/', 'core/model_import/')
TOP = len(LAYERS)
SOURCE_SUFFIXES = {'.h', '.hpp', '.inl', '.c', '.cpp'}
SKIPPED = {'lib3dparty', 'build', '.git'}

INCLUDE = re.compile(r'^\s*#\s*include\s*<snk/([A-Za-z0-9_]+)/')
ALIAS = re.compile(r'add_library\(\s*(snk::[A-Za-z0-9_]+)\s+ALIAS\b')
LINKS = re.compile(r'target_link_libraries\s*\((.*?)\)', re.S)
INCLUDE_DIRS = re.compile(r'target_include_directories\s*\((.*?)\)', re.S)
TARGET = re.compile(r'snk::[A-Za-z0-9_]+')
# ${ENGINE_CORE_PATH} is <repo>/core: a path written under it names its layer by what follows.
CORE_PATH = re.compile(r'\$\{ENGINE_CORE_PATH\}/(core|engine|model_import)/')


def layer_name(layer):
    return LAYERS[layer].rstrip('/') if layer < TOP else 'Editor/tools/apps/tests'


def layer_of(relative):
    """The layer index of a repository path (posix, relative): 0..2 under core/, TOP elsewhere."""
    for index, prefix in enumerate(LAYERS):
        if relative.startswith(prefix):
            return index
    return TOP


def code_lines(text):
    """The lines of a C++ or CMake file without // and # comments (enough for the lines this script reads)."""
    for number, line in enumerate(text.splitlines(), 1):
        yield number, line.split('//', 1)[0]


def include_problems(path, text, owners):
    """Violations of one source file: `owners` maps a snk/<name> to the lowest layer that declares it."""
    own = layer_of(path)
    out = []
    for number, line in code_lines(text):
        match = INCLUDE.match(line)
        if not match:
            continue
        owner = owners.get(match.group(1))
        if owner is not None and owner > own:
            out.append(f'{path}:{number}: includes <snk/{match.group(1)}/...> of {layer_name(owner)} '
                       f'from {layer_name(own)}')
    return out


def cmake_problems(path, text, targets):
    """Violations of one CMakeLists.txt: `targets` maps a snk:: target to the layer that defines it."""
    own = layer_of(path)
    if own == TOP:
        return []
    body = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
    out = []
    for call in LINKS.findall(body):
        for name in TARGET.findall(call):
            layer = targets.get(name)
            if layer is not None and layer > own:
                out.append(f'{path}: links {name} of {layer_name(layer)} from {layer_name(own)}')
    for call in INCLUDE_DIRS.findall(body):
        for sub in CORE_PATH.findall(call):
            layer = layer_of(f'core/{sub}/')
            if layer > own:
                out.append(f'{path}: include directory of core/{sub} from {layer_name(own)}')
    return out


LINK_KEYWORDS = ('PUBLIC', 'PRIVATE', 'INTERFACE')
PUBLIC_INCLUDE = re.compile(r'^\s*#\s*include\s*<snk/([A-Za-z0-9_]+)/([^>]*)>', re.M)
# Targets whose headers come from an aggregate on purpose (their CMake links snk::engine_api / is the aggregate).
AGGREGATE_USERS = {'snk::engine', 'snk::model_import_assimp'}
# (target, component it includes) left hidden, with the reason.
HIDDEN_EXCEPTIONS = {}


def links_of(text):
    """The snk:: targets a CMakeLists.txt links, with the strongest visibility it names them with."""
    body = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
    links = {}
    for call in LINKS.findall(body):
        visibility = 'PRIVATE'
        for token in call.split()[1:]:
            if token in LINK_KEYWORDS:
                visibility = token
                continue
            for name in TARGET.findall(token):
                if links.get(name) not in ('PUBLIC', 'INTERFACE'):
                    links[name] = visibility
    return links


def public_closure(target, links_by_target):
    reached, stack = set(), [target]
    while stack:
        for name, visibility in links_by_target.get(stack.pop(), {}).items():
            if visibility in ('PUBLIC', 'INTERFACE') and name not in reached:
                reached.add(name)
                stack.append(name)
    return reached


def hidden_problems(targets, owners_of_names, headers_of, exceptions=None):
    """`targets`: alias -> links; `owners_of_names`: snk/<name> -> alias; `headers_of`: alias -> [(path, text)]."""
    exceptions = HIDDEN_EXCEPTIONS if exceptions is None else exceptions
    out = []
    for target in sorted(targets):
        if target in AGGREGATE_USERS:
            continue
        reach = public_closure(target, targets) | {target}
        for path, text in headers_of.get(target, []):
            for name, rest in PUBLIC_INCLUDE.findall(text):
                if name == 'common' and rest == 'snk_api.h' and 'snk::api' in targets[target]:
                    continue
                owner = owners_of_names.get(name)
                if owner is None or owner in reach or (target, owner) in exceptions:
                    continue
                out.append(f'{path}: a public header of {target} includes <snk/{name}/...> of {owner}, '
                           f'which {target} does not link PUBLIC')
    return out


def hidden_dependencies(root):
    targets, owners_of_names, headers_of = {}, {}, {}
    for cmake in sorted(root.glob('core/**/CMakeLists.txt')):
        if 'lib3dparty' in cmake.parts:
            continue
        text = cmake.read_text(encoding='utf-8', errors='replace')
        aliases = ALIAS.findall(text)
        if not aliases:
            continue
        target = aliases[0]
        targets[target] = links_of(text)
        headers_of[target] = []
        for sub in ('include', 'include_base'):
            folder = cmake.parent / sub
            if not folder.is_dir():
                continue
            if (folder / 'snk').is_dir():
                for named in (folder / 'snk').iterdir():
                    if named.is_dir():
                        owners_of_names.setdefault(named.name, target)
            for header in sorted(folder.rglob('*')):
                if header.is_file() and header.suffix in ('.h', '.hpp', '.inl'):
                    headers_of[target].append((header.relative_to(root).as_posix(),
                                               header.read_text(encoding='utf-8', errors='replace')))
    return hidden_problems(targets, owners_of_names, headers_of)


def walk(root):
    for path in sorted(root.rglob('*')):
        parts = path.relative_to(root).parts
        if parts and parts[0] in SKIPPED or 'lib3dparty' in parts:
            continue
        if path.is_file():
            yield path


def owners_of(root):
    owners = {}
    for path in root.rglob('*'):
        relative = path.relative_to(root).as_posix()
        if 'lib3dparty' in relative or relative.startswith(('build/', '.git/')) or not path.is_dir():
            continue
        match = re.search(r'(.*)/include(?:_base)?/snk/([A-Za-z0-9_]+)$', relative)
        if match:
            layer = layer_of(match.group(1) + '/')
            owners[match.group(2)] = min(layer, owners.get(match.group(2), layer))
    return owners


def targets_of(root):
    targets = {}
    for path in walk(root):
        if path.name != 'CMakeLists.txt':
            continue
        relative = path.relative_to(root).as_posix()
        # A test project may define an alias of the engine again (tools/ci/engine_copies_toy): the lowest layer wins.
        layer = layer_of(relative)
        for name in ALIAS.findall(path.read_text(encoding='utf-8', errors='replace')):
            targets[name] = min(layer, targets.get(name, layer))
    return targets


def check(root):
    root = Path(root)
    owners, targets = owners_of(root), targets_of(root)
    problems = []
    for path in walk(root):
        relative = path.relative_to(root).as_posix()
        if not relative.startswith('core/'):
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        if path.suffix in SOURCE_SUFFIXES:
            problems += include_problems(relative, text, owners)
        elif path.name == 'CMakeLists.txt':
            problems += cmake_problems(relative, text, targets)
    return problems + hidden_dependencies(root)


def self_test():
    owners = {'common': 0, 'resource': 0, 'render': 1, 'model_import': 2, 'editor': TOP}
    targets = {'snk::common': 0, 'snk::render': 1, 'snk::model_import_core': 2, 'snk::editor_api': TOP}
    cases = [
        ('core includes core', include_problems('core/core/a/x.cpp', '#include <snk/common/a.h>\n', owners), 0),
        ('engine includes core', include_problems('core/engine/r/x.h', '#include <snk/resource/res_tag.h>\n', owners), 0),
        ('core includes engine', include_problems('core/core/a/x.cpp', '#include <snk/render/r.h>\n', owners), 1),
        ('engine includes the import', include_problems('core/engine/a/x.h', '# include <snk/model_import/m.h>\n', owners), 1),
        ('the import includes the editor', include_problems('core/model_import/x.cpp', '#include <snk/editor/e.h>\n', owners), 1),
        ('a comment is no include', include_problems('core/core/a/x.cpp', '// #include <snk/render/r.h>\n', owners), 0),
        ('an unknown name', include_problems('core/core/a/x.cpp', '#include <snk/other/o.h>\n', owners), 0),
        ('core links core', cmake_problems('core/core/a/CMakeLists.txt', 'target_link_libraries(a PRIVATE snk::common)', targets), 0),
        ('core links engine', cmake_problems('core/core/a/CMakeLists.txt',
                                             'target_link_libraries(a\n\tPUBLIC snk::common PRIVATE snk::render)', targets), 1),
        ('a commented link', cmake_problems('core/core/a/CMakeLists.txt', '# target_link_libraries(a snk::render)', targets), 0),
        ('engine links the import', cmake_problems('core/engine/CMakeLists.txt',
                                                   'target_link_libraries(e PRIVATE snk::model_import_core)', targets), 1),
        ('core includes an engine directory', cmake_problems('core/core/m/CMakeLists.txt',
            'target_include_directories(m PUBLIC "$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/engine/scene/include>")', targets), 1),
        ('the import includes an engine directory', cmake_problems('core/model_import/CMakeLists.txt',
            'target_include_directories(m PUBLIC "$<BUILD_INTERFACE:${ENGINE_CORE_PATH}/engine/scene/include>")', targets), 0),
        ('the editor is not checked', cmake_problems('Editor/CMakeLists.txt', 'target_link_libraries(e snk::editor_api)', targets), 0),
    ]
    links = {'snk::a': {'snk::b': 'PRIVATE', 'snk::api': 'PUBLIC'}, 'snk::b': {}, 'snk::c': {'snk::b': 'PUBLIC'},
             'snk::d': {'snk::c': 'PUBLIC'}, 'snk::drv_opengl': {}, 'snk::window': {}}
    owned = {'b': 'snk::b', 'common': 'snk::common', 'window': 'snk::window'}
    cases += [
        ('a hidden dependency', hidden_problems(links, owned, {'snk::a': [('a.h', '#include <snk/b/x.h>\n')]}), 1),
        ('a public link', hidden_problems(links, owned, {'snk::c': [('c.h', '#include <snk/b/x.h>\n')]}), 0),
        ('through a public link', hidden_problems(links, owned, {'snk::d': [('d.h', '#include <snk/b/x.h>\n')]}), 0),
        ('snk_api.h through snk::api', hidden_problems(links, owned, {'snk::a': [('a.h', '#include <snk/common/snk_api.h>\n')]}), 0),
        ('another common header', hidden_problems(links, owned, {'snk::a': [('a.h', '#include <snk/common/x.h>\n')]}), 1),
        ('a listed exception', hidden_problems(links, owned, {'snk::drv_opengl': [('g.h', '#include <snk/window/w.h>\n')]},
                                               {('snk::drv_opengl', 'snk::window'): 'a reason'}), 0),
        ('links are read with their visibility', [] if links_of(
            'target_link_libraries(x PRIVATE snk::b PUBLIC snk::c $<BUILD_LOCAL_INTERFACE:snk::d>)') ==
            {'snk::b': 'PRIVATE', 'snk::c': 'PUBLIC', 'snk::d': 'PUBLIC'} else ['wrong'], 0),
    ]
    failed = 0
    for name, problems, expected in cases:
        if len(problems) != expected:
            failed += 1
            print(f'check_layers self-test: "{name}": expected {expected} problem(s), got {problems}')
    if failed:
        return 1
    print(f'check_layers: self-test passed ({len(cases)} cases)')
    return 0


def main(argv):
    if '--self-test' in argv:
        return self_test()
    root = argv[argv.index('--repo') + 1] if '--repo' in argv else '.'
    problems = check(root)
    for problem in problems:
        print(f'check_layers: {problem}')
    if problems:
        print(f'check_layers: {len(problems)} problem(s): an edge from a lower layer to a higher one, or a public header '
              f'including what its target does not link PUBLIC; layers: '
              f'{" < ".join(layer_name(index) for index in range(TOP + 1))}')
        return 1
    print('check_layers: no lower layer includes or links a higher one, no hidden public dependency')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
