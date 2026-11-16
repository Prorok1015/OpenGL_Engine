#!/usr/bin/env python3
"""Fails when a change of the module ABI brings no entry in docs/abi/changes/ (#1056).

    check_abi_changes.py --base <commit> [--head <commit>] [--repo <root>]   check the changes from <base> to <head>
    check_abi_changes.py --state [--repo <root>]   without a base: the current layout_revision is told in docs/abi
    check_abi_changes.py --self-test
    (defaults: --head HEAD, --repo .)

The history of the module ABI was one comment above SNK_MODULE_API_VERSION: two pull requests that each added a line
to its end always conflicted. Each change now adds its own file, docs/abi/changes/<issue>.md, so two changes never
touch one line. A change of the ABI is, between <base> and <head>:
  - another value of gs::detail::layout_revision (gs_module_layout.h);
  - another SNK_MODULE_API_VERSION (app_module_entry.h) or SNK_EDITOR_API_VERSION (edt_editor_extension.h);
  - another tools/ci/vtable_pins.json;
  - an added line that holds SNK_API or SNK_EDITOR_API in any header (.h, .hpp, .inl; exported headers also live in
    include_base/ and next to sources), comments left out, the two headers that define the macros left out. A line
    the change also removes elsewhere (a declaration moved between headers) does not count.
Such a change needs at least one added .md file in docs/abi/changes/ other than README.md; with another layout_revision
one of them names the new value as `layout_revision <N>`. A new export inside a class that is SNK_API already adds no
such line: the script does not see it, the entry is then the author's to write.
Lines added to the history comment above SNK_MODULE_API_VERSION are refused: that history moved to docs/abi.
"""
import re
import subprocess
import sys
from pathlib import Path

LAYOUT_HEADER = 'core/engine/game_system/include/snk/game/gs_module_layout.h'
MODULE_ENTRY = 'core/core/application/include/snk/application/app_module_entry.h'
EDITOR_EXTENSION = 'Editor/code/editor_system/include/snk/editor/edt_editor_extension.h'
PINS = 'tools/ci/vtable_pins.json'
CHANGES_DIR = 'docs/abi/changes/'
TOOL = 'check_abi_changes'

REVISION = re.compile(r'layout_revision\s*=\s*(\d+)')
MODULE_VERSION = re.compile(r'#define\s+SNK_MODULE_API_VERSION\s+(\S+)')
EDITOR_VERSION = re.compile(r'#define\s+SNK_EDITOR_API_VERSION\s+(\S+)')
EXPORT = re.compile(r'\bSNK_(?:EDITOR_)?API\b')


MACRO_HEADERS = ('core/core/common/include/snk/common/snk_api.h', 'Editor/code/editor_system/include/snk/editor/edt_api.h')
HEADER_SUFFIXES = ('.h', '.hpp', '.inl')
THIRD_PARTY = 'lib3dparty/'


def code_of(line):
    """The line without its // comment and its /* */ comments that close on it."""
    line = re.sub(r'/\*.*?\*/', ' ', line)
    return line.split('//', 1)[0]


def normalized(line):
    return ' '.join(code_of(line).split())


def is_watched_header(path):
    return path.endswith(HEADER_SUFFIXES) and not path.startswith(THIRD_PARTY) and path not in MACRO_HEADERS


def first_match(pattern, text):
    if text is None:
        return None
    found = pattern.search(text)
    return found.group(1) if found else None


def history_lines(text):
    """The comment lines right above #define SNK_MODULE_API_VERSION."""
    if text is None:
        return []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if MODULE_VERSION.search(line):
            block = []
            at = index - 1
            while at >= 0 and lines[at].lstrip().startswith('//'):
                block.append(lines[at])
                at -= 1
            return block
    return []


def evaluate(change):
    """The problems of one change. `change` holds, for base and head: the texts of the three headers and of the pin
    file (None when missing); `added_lines` / `removed_lines`: {path: [lines]} added and removed in headers; `added_files`:
    {path: text} of files added under docs/abi/changes/."""
    reasons = []
    base_revision = first_match(REVISION, change['base'].get(LAYOUT_HEADER))
    head_revision = first_match(REVISION, change['head'].get(LAYOUT_HEADER))
    if base_revision != head_revision:
        reasons.append(f'layout_revision {base_revision} -> {head_revision}')
    for path, pattern, name in ((MODULE_ENTRY, MODULE_VERSION, 'SNK_MODULE_API_VERSION'),
                                (EDITOR_EXTENSION, EDITOR_VERSION, 'SNK_EDITOR_API_VERSION')):
        before = first_match(pattern, change['base'].get(path))
        after = first_match(pattern, change['head'].get(path))
        if before != after:
            reasons.append(f'{name} {before} -> {after}')
    if change['base'].get(PINS) != change['head'].get(PINS):
        reasons.append(f'{PINS} changed')
    removed = {normalized(line) for lines in change.get('removed_lines', {}).values() for line in lines}
    for path, lines in sorted(change['added_lines'].items()):
        if not is_watched_header(path):
            continue
        if any(EXPORT.search(code_of(line)) and normalized(line) not in removed for line in lines):
            reasons.append(f'{path} adds an export')

    problems = []
    grown = len(history_lines(change['head'].get(MODULE_ENTRY))) > len(history_lines(change['base'].get(MODULE_ENTRY)))
    if grown:
        problems.append(f'{MODULE_ENTRY}: the comment above SNK_MODULE_API_VERSION grew; the history is in docs/abi, '
                        f'add {CHANGES_DIR}<issue>.md instead')
    entries = {path: text for path, text in change['added_files'].items()
               if path.startswith(CHANGES_DIR) and path.endswith('.md') and Path(path).name != 'README.md'}
    if reasons and not entries:
        problems.append('the module ABI changes (' + '; '.join(reasons) + f') but no file is added to {CHANGES_DIR}: '
                        f'write {CHANGES_DIR}<issue>.md ({CHANGES_DIR}README.md says what goes in it)')
    if entries and head_revision is not None and base_revision != head_revision:
        named = re.compile(rf'layout_revision\D{{0,3}}{re.escape(head_revision)}\b')
        if not any(named.search(text) for text in entries.values()):
            problems.append(f'layout_revision is {head_revision} now: an entry in {CHANGES_DIR} must say '
                            f'`layout_revision {head_revision}`')
    return problems


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, text=True, encoding='utf-8',
                          errors='replace').stdout


def text_at(repo, commit, path):
    try:
        return git(repo, 'show', f'{commit}:{path}')
    except subprocess.CalledProcessError:
        return None


def collect(repo, base, head):
    watched = (LAYOUT_HEADER, MODULE_ENTRY, EDITOR_EXTENSION, PINS)
    change = {
        'base': {path: text_at(repo, base, path) for path in watched},
        'head': {path: text_at(repo, head, path) for path in watched},
        'added_lines': {},
        'removed_lines': {},
        'added_files': {},
    }
    for line in git(repo, 'diff', '--name-status', '--no-renames', base, head).splitlines():
        status, _, path = line.partition('\t')
        if status == 'A' and path.startswith(CHANGES_DIR):
            change['added_files'][path] = text_at(repo, head, path) or ''
        if path.endswith(HEADER_SUFFIXES) and not path.startswith(THIRD_PARTY) and status in ('A', 'M', 'D'):
            diff = git(repo, 'diff', '--unified=0', base, head, '--', path).splitlines()
            change['added_lines'][path] = [l[1:] for l in diff if l.startswith('+') and not l.startswith('+++')]
            change['removed_lines'][path] = [l[1:] for l in diff if l.startswith('-') and not l.startswith('---')]
    return change


def state_problems(layout_text, entries, history):
    """Without a base: the current layout_revision is told in an entry or in the history."""
    revision = first_match(REVISION, layout_text)
    if revision is None:
        return [f'{LAYOUT_HEADER}: no layout_revision found']
    in_entry = re.compile(rf'layout_revision\D{{0,3}}{re.escape(revision)}\b')
    in_history = re.compile(rf'(?m)^{re.escape(revision)}:')
    if any(in_entry.search(text) for text in entries) or (history and in_history.search(history)):
        return []
    return [f'layout_revision is {revision}, but no entry in {CHANGES_DIR} and no line of docs/abi/HISTORY.md tells it']


def check_state(repo):
    root = Path(repo)
    entries = [p.read_text(encoding='utf-8', errors='replace') for p in sorted((root / CHANGES_DIR).glob('*.md'))
               if p.name != 'README.md']
    history_path = root / 'docs/abi/HISTORY.md'
    history = history_path.read_text(encoding='utf-8', errors='replace') if history_path.exists() else None
    layout = (root / LAYOUT_HEADER).read_text(encoding='utf-8', errors='replace')
    return state_problems(layout, entries, history)


def self_test():
    def change(**over):
        base = {LAYOUT_HEADER: 'inline constexpr std::uint64_t layout_revision = 14;',
                MODULE_ENTRY: '// rule\n#define SNK_MODULE_API_VERSION 0x0004\'0000u\n',
                EDITOR_EXTENSION: '#define SNK_EDITOR_API_VERSION 0x0002\'0000u\n', PINS: '{}'}
        head = dict(base)
        head.update(over.get('head', {}))
        return {'base': base, 'head': head, 'added_lines': over.get('added_lines', {}),
                'removed_lines': over.get('removed_lines', {}), 'added_files': over.get('added_files', {})}

    cases = [
        ('nothing changes', change(), 0),
        ('a comment changes', change(added_lines={'a/include/x.h': ['// note']}), 0),
        ('a raised layout_revision without an entry',
         change(head={LAYOUT_HEADER: 'inline constexpr std::uint64_t layout_revision = 15;'}), 1),
        ('a raised layout_revision with an entry that names it',
         change(head={LAYOUT_HEADER: 'inline constexpr std::uint64_t layout_revision = 15;'},
                added_files={CHANGES_DIR + '1100.md': 'x gains y at its end (layout_revision 15).'}), 0),
        ('a raised layout_revision with an entry that names another value',
         change(head={LAYOUT_HEADER: 'inline constexpr std::uint64_t layout_revision = 15;'},
                added_files={CHANGES_DIR + '1100.md': 'x gains y (layout_revision 14).'}), 1),
        ('a new export without an entry', change(added_lines={'core/x/include/snk/x.h': ['SNK_API void f();']}), 1),
        ('a new export in include_base without an entry',
         change(added_lines={'core/core/common/include_base/snk/common/x.h': ['class SNK_API x {};']}), 1),
        ('a new export in a header next to sources', change(added_lines={'core/engine/x/x_impl.h': ['SNK_API int g();']}), 1),
        ('SNK_API in a comment', change(added_lines={'core/x/include/snk/x.h': ['// SNK_API is not needed here',
                                                                               'int f(); /* SNK_API later */']}), 0),
        ('the macro headers', change(added_lines={MACRO_HEADERS[0]: ['#define SNK_API __declspec(dllexport)']}), 0),
        ('a third-party header', change(added_lines={'lib3dparty/x/x.h': ['SNK_API void f();']}), 0),
        ('a declaration moved between headers',
         change(added_lines={'core/x/include/snk/b.h': ['\tSNK_API void f(int a);']},
                removed_lines={'core/x/include/snk/a.h': ['SNK_API  void f(int a);']}), 0),
        ('a new editor export with an entry',
         change(added_lines={'Editor/include/snk/editor/x.h': ['SNK_EDITOR_API void f();']},
                added_files={CHANGES_DIR + '1101.md': 'f is a new export.'}), 0),
        ('only a README in the folder', change(head={PINS: '{"a": 1}'}, added_files={CHANGES_DIR + 'README.md': 'x'}), 1),
        ('another editor API version without an entry',
         change(head={EDITOR_EXTENSION: '#define SNK_EDITOR_API_VERSION 0x0003\'0000u\n'}), 1),
        ('a line added to the old history comment',
         change(head={MODULE_ENTRY: '// rule\n// #1100: x\n#define SNK_MODULE_API_VERSION 0x0004\'0000u\n'},
                added_files={CHANGES_DIR + '1100.md': 'x'}), 1),
    ]
    state_cases = [
        ('the revision is in the history', state_problems('layout_revision = 14;', [], '13: x\n14: y\n'), 0),
        ('the revision is in an entry', state_problems('layout_revision = 15;', ['z (layout_revision 15).'], '14: y\n'), 0),
        ('the revision is told nowhere', state_problems('layout_revision = 15;', ['no revision'], '14: y\n'), 1),
    ]
    failed = 0
    for name, problems, expected in state_cases:
        if len(problems) != expected:
            failed += 1
            print(f'{TOOL} self-test: "{name}": expected {expected} problem(s), got {problems}')
    for name, case, expected in cases:
        problems = evaluate(case)
        if len(problems) != expected:
            failed += 1
            print(f'{TOOL} self-test: "{name}": expected {expected} problem(s), got {problems}')
    if failed:
        return 1
    print(f'{TOOL}: self-test passed ({len(cases) + len(state_cases)} cases)')
    return 0


def main(argv):
    if '--self-test' in argv:
        return self_test()
    if '--state' in argv:
        repo = argv[argv.index('--repo') + 1] if '--repo' in argv else '.'
        problems = check_state(repo)
        for problem in problems:
            print(f'{TOOL}: {problem}')
        if problems:
            return 1
        print(f'{TOOL}: no base to compare with: the current layout_revision is told in docs/abi')
        return 0
    options = {'--repo': '.', '--head': 'HEAD', '--base': None}
    it = iter(argv)
    for arg in it:
        if arg not in options:
            print(__doc__)
            return 2
        options[arg] = next(it, None)
    if not options['--base']:
        print(__doc__)
        return 2
    problems = evaluate(collect(Path(options['--repo']), options['--base'], options['--head']))
    for problem in problems:
        print(f'{TOOL}: {problem}')
    if problems:
        return 1
    print(f'{TOOL}: ABI changes have their entries in {CHANGES_DIR}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
