#!/usr/bin/env python3
"""Fails when a public header of the SDK holds a byte that is not ASCII (#219).

    check_ascii_headers.py --repo <root>       the headers under */include/snk/ of the sources
    check_ascii_headers.py --installed <dir>   the headers under <dir> (an installed include/snk), bundled third-party left out
    check_ascii_headers.py --self-test

A game module is compiled without the engine's /utf-8, in the code page of the user's machine: a character outside it
in a header (even in a comment) is C4819, and an error under the module's own /WX.
"""
import sys
from pathlib import Path

HEADER_SUFFIXES = {'.h', '.hpp', '.inl'}
# Installed next to the engine's headers, but third-party code with its own rules.
THIRD_PARTY = {'lib3dparty', 'boost'}


def problems_of(path, text):
    out = []
    for number, line in enumerate(text.splitlines(), 1):
        for column, byte in enumerate(line, 1):
            if byte > 0x7F:
                out.append(f'{path}:{number}:{column}: byte 0x{byte:02X} is not ASCII')
                break
    return out


def repo_headers(root):
    for path in sorted(root.rglob('*')):
        parts = path.relative_to(root).parts
        if 'lib3dparty' in parts or parts[0] in ('build', '.git'):
            continue
        if path.suffix in HEADER_SUFFIXES and path.is_file() and '/include/snk/' in path.as_posix():
            yield path


def installed_headers(root):
    for path in sorted(root.rglob('*')):
        if path.relative_to(root).parts[0] in THIRD_PARTY:
            continue
        if path.suffix in HEADER_SUFFIXES and path.is_file():
            yield path


def check(paths):
    errors = []
    count = 0
    for path in paths:
        count += 1
        errors += problems_of(path, path.read_bytes())
    return count, errors


def self_test():
    assert problems_of('a.h', b'// plain\nint x;\n') == []
    assert problems_of('a.h', '// a — b\n'.encode()) == ['a.h:1:6: byte 0xE2 is not ASCII']
    assert problems_of('a.h', b'int y;\n"\xa7"\n') == ['a.h:2:2: byte 0xA7 is not ASCII']
    print('check_ascii_headers: self-test passed')


def main(argv):
    if argv == ['--self-test']:
        self_test()
        return 0
    if len(argv) != 2 or argv[0] not in ('--repo', '--installed'):
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(argv[1])
    count, errors = check(repo_headers(root) if argv[0] == '--repo' else installed_headers(root))
    for error in errors:
        print(error)
    if count == 0:
        print(f'no headers found under {root}')
        return 1
    if errors:
        print(f'{len(errors)} public header(s) hold non-ASCII bytes: game modules compile them in the code page of the user\'s machine')
        return 1
    print(f'{count} public headers, all ASCII')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
