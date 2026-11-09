#!/usr/bin/env python3
"""Fails when the shared set of an installed SDK makes a consumer link an archive next to snk_engine (#581).

    check_sdk_shared_set.py <sdk prefix> [--allow <export name>...]
    check_sdk_shared_set.py --self-test

snk_engine carries the code of every engine component and bundled library, so the shared set exports them
usage-only (cmake/SnkInstall.cmake, _snk_sdk_make_facade). Linking one of their archives as well would put a
second copy of its code and globals next to the DLL's. Checked in <prefix>/lib/cmake/snake_sdk/snake_sdk_sharedTargets.cmake:
  1. every STATIC IMPORTED target is on the allow list (--allow, without the snk:: namespace): on Windows the
     archives a game module links itself (Boost.JSON, SPIKE-218 section 3.6);
  2. snk::engine has no interface links, and every interface link of snk::engine_api and snk::core_api is a
     $<COMPILE_ONLY:...> item (the plan of #581: a consumer links the engine alone);
  3. nothing a consumer of snk::engine and snk::engine_api links (plain and $<LINK_ONLY:...> items, transitively;
     $<COMPILE_ONLY:...> does not link) is a STATIC target;
and <prefix>/lib/snk/ holds no archive but the engine's import library and the allowed ones.
"""
import re
import sys
from pathlib import Path

TARGETS_FILE = Path('lib/cmake/snake_sdk/snake_sdk_sharedTargets.cmake')
ENTRY_POINTS = ('snk::engine', 'snk::engine_api', 'snk::editor_api')
USAGE_ONLY = ('snk::engine_api', 'snk::core_api')
ARCHIVE_SUFFIXES = ('.lib', '.a')


def parse_targets(text):
    types = dict(re.findall(r'add_library\((snk::[\w.+-]+) (\w+) IMPORTED\)', text))
    links = {}
    for m in re.finditer(r'set_target_properties\((snk::[\w.+-]+) PROPERTIES\n(.*?)\n\)', text, re.S):
        found = re.search(r'INTERFACE_LINK_LIBRARIES "(.*)"', m.group(2))
        if found:
            links[m.group(1)] = found.group(1).replace('\\$', '$').split(';')
    return types, links


def linked_targets(item):
    """The snk:: targets a link item makes the consumer link."""
    if item.startswith('$<COMPILE_ONLY:'):
        return []
    return re.findall(r'snk::[\w.+-]+', item)


def check_targets(text, allowed):
    errors = []
    types, links = parse_targets(text)
    if not types:
        return ['no imported targets found']
    for name, kind in sorted(types.items()):
        if kind == 'STATIC' and name[len('snk::'):] not in allowed:
            errors.append(f'{name} is a STATIC target of the shared set (allowed: {", ".join(sorted(allowed)) or "none"})')
    if 'snk::engine' not in types:
        errors.append('snk::engine is not exported')
    elif links.get('snk::engine'):
        errors.append(f'snk::engine has interface links: {";".join(links["snk::engine"])}')
    # Without this a parse that stopped matching the export's format would pass every rule on empty link lists.
    api_links = links.get('snk::engine_api', [])
    if '$<COMPILE_ONLY:snk::core_api>' not in api_links:
        errors.append('snk::engine_api does not link $<COMPILE_ONLY:snk::core_api>: the export changed its format, '
                      'or its links were lost')
    for name in USAGE_ONLY:
        if name not in types:
            errors.append(f'{name} is not exported')
        for item in links.get(name, []):
            if not item.startswith('$<COMPILE_ONLY:'):
                errors.append(f'{name} links {item}, not $<COMPILE_ONLY:...>')
    seen = set()
    todo = [t for t in ENTRY_POINTS if t in types]
    while todo:
        current = todo.pop()
        if current in seen:
            continue
        seen.add(current)
        for item in links.get(current, []):
            for dep in linked_targets(item):
                if types.get(dep) == 'STATIC':
                    errors.append(f'a consumer of {" + ".join(ENTRY_POINTS)} links the archive {dep} (via {current})')
                todo.append(dep)
    return errors


def imported_files(config_texts):
    """{file name: target} of the IMPORTED_LOCATION/IMPORTED_IMPLIB of every target, all configurations."""
    files = {}
    for text in config_texts:
        for m in re.finditer(r'set_target_properties\((snk::[\w.+-]+) PROPERTIES\n(.*?)\n *\)', text, re.S):
            for path in re.findall(r'IMPORTED_(?:LOCATION|IMPLIB)_\w+ "([^"]+)"', m.group(2)):
                files[path.rsplit('/', 1)[-1]] = m.group(1)
    return files


def check_archives(lib_dir, files, allowed):
    errors = []
    if not lib_dir.is_dir():
        return errors
    owners = {'snk::engine', 'snk::editor_api'} | {f'snk::{name}' for name in allowed}
    for path in sorted(lib_dir.iterdir()):
        if path.is_file() and path.suffix in ARCHIVE_SUFFIXES and files.get(path.name) not in owners:
            errors.append(f'{path} is installed by the shared set ({files.get(path.name, "no target")})')
    return errors


def self_test():
    # As install(EXPORT) writes it: `$` escaped, include directories before the links, a facade's plain links
    # (other facades, an imported system target) and a kept archive (MSVC) used compile-only.
    good = '''add_library(snk::engine SHARED IMPORTED)

add_library(snk::engine_api INTERFACE IMPORTED)

set_target_properties(snk::engine_api PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
  INTERFACE_LINK_LIBRARIES "\\$<COMPILE_ONLY:snk::core_api>;\\$<COMPILE_ONLY:snk::scene>"
)

add_library(snk::core_api INTERFACE IMPORTED)

set_target_properties(snk::core_api PROPERTIES
  INTERFACE_LINK_LIBRARIES "\\$<COMPILE_ONLY:snk::ecs>"
)

add_library(snk::scene INTERFACE IMPORTED)

set_target_properties(snk::scene PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS "GLM_FORCE_RADIANS"
  INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
  INTERFACE_LINK_LIBRARIES "snk::ecs;snk::thirdparty_glm;Threads::Threads;\\$<COMPILE_ONLY:snk::thirdparty_boost_json>"
)

add_library(snk::ecs INTERFACE IMPORTED)

add_library(snk::thirdparty_glm INTERFACE IMPORTED)

add_library(snk::thirdparty_boost_json STATIC IMPORTED)
'''
    assert parse_targets(good)[1]['snk::scene'][-1] == '$<COMPILE_ONLY:snk::thirdparty_boost_json>'
    assert check_targets(good, {'thirdparty_boost_json'}) == []
    assert len(check_targets(good, set())) == 1  # boost_json not allowed
    # A facade links the archive plainly, and engine_api reaches it plainly.
    linked = good.replace('\\$<COMPILE_ONLY:snk::scene>', 'snk::scene').replace(
        '\\$<COMPILE_ONLY:snk::thirdparty_boost_json>', 'snk::thirdparty_boost_json')
    assert linked != good
    errors = check_targets(linked, {'thirdparty_boost_json'})
    assert any('links the archive' in e for e in errors)
    assert any('snk::engine_api links snk::scene' in e for e in errors)
    static_scene = good.replace('add_library(snk::scene INTERFACE', 'add_library(snk::scene STATIC')
    assert len(check_targets(static_scene, {'thirdparty_boost_json'})) == 1
    engine_links = good.replace('add_library(snk::engine SHARED IMPORTED)\n', '''add_library(snk::engine SHARED IMPORTED)

set_target_properties(snk::engine PROPERTIES
  INTERFACE_LINK_LIBRARIES "Threads::Threads"
)
''')
    assert any('snk::engine has interface links' in e for e in check_targets(engine_links, {'thirdparty_boost_json'}))
    unparsed = re.sub(r'set_target_properties\(snk::engine_api PROPERTIES\n.*?\n\)\n', '', good, flags=re.S)
    assert unparsed != good
    assert any('does not link $<COMPILE_ONLY:snk::core_api>' in e for e in check_targets(unparsed, {'thirdparty_boost_json'}))
    core_plain = good.replace('\\$<COMPILE_ONLY:snk::ecs>', 'snk::ecs')
    assert any('snk::core_api links snk::ecs' in e for e in check_targets(core_plain, {'thirdparty_boost_json'}))
    config = '''set_target_properties(snk::thirdparty_boost_json PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/snk/libboost_json-vc145-mt-x64-1_84.lib"
  )
set_target_properties(snk::engine PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/snk/snk_engine.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/snk_engine.dll"
  )
'''
    files = imported_files([config])
    assert files['libboost_json-vc145-mt-x64-1_84.lib'] == 'snk::thirdparty_boost_json'
    assert files['snk_engine.lib'] == 'snk::engine'
    # The editor API library (#587) is a shared library of the set: its import library is no stray archive.
    with_editor = config + '''set_target_properties(snk::editor_api PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/snk/snk_editor_api.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/snk_editor_api.dll"
  )
'''
    assert imported_files([with_editor])['snk_editor_api.lib'] == 'snk::editor_api'
    print('check_sdk_shared_set: self-test passed')


def main(argv):
    if argv[1:] == ['--self-test']:
        self_test()
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    prefix = Path(argv[1])
    allowed = set()
    if len(argv) > 2:
        if argv[2] != '--allow':
            print(__doc__)
            return 2
        allowed = set(argv[3:])
    targets = prefix / TARGETS_FILE
    if not targets.is_file():
        print(f'::error::{targets} does not exist; is {prefix} an installed SDK with the shared engine?')
        return 2
    errors = check_targets(targets.read_text(encoding='utf-8'), allowed)
    configs = [p.read_text(encoding='utf-8') for p in targets.parent.glob('snake_sdk_sharedTargets-*.cmake')]
    errors += check_archives(prefix / 'lib' / 'snk', imported_files(configs), allowed)
    for error in errors:
        print(f'::error::{error}')
    print(f'check_sdk_shared_set: {targets}: {len(errors)} problem(s)')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
