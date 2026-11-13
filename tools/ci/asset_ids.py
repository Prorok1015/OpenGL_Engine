#!/usr/bin/env python3
"""The .resource sidecars of the repository's resource trees (EPIC-57 S57-6, #768).

    asset_ids.py --write <tree>...   make a .resource for every asset file of the trees that has none
    asset_ids.py --check <tree>...   fail when an asset file has no .resource or a .resource is not what the engine writes
    asset_ids.py --self-test

Every file of a resource tree has a .resource next to it that holds its asset ID (EPIC-57 section 4.2). The editor makes
them for a project; the trees of the repository (core/res, projects/sandbox/res, tests/static_game/project/res) get
theirs from this script once, are committed, and CI keeps them complete.

--write gives a .desc the "__guid" of its root object (EPIC-57 section 8: a level's __guid is its ID already) unless
another file of the trees holds that ID, any other file a new random ID. A .resource that exists is never changed, so a
second run changes nothing.

--check reports, one line each:
  - an asset file without a .resource, and a .resource without its file;
  - a .resource that res::read_asset_meta would refuse, or whose bytes are not what res::write_asset_meta writes for
    the same values (the one JSON style, LF line ends);
  - a .desc whose .resource holds another ID than the "__guid" of its root;
  - one ID held by two .resource files of the given trees.

The walk is the editor's (edt::asset_database): hidden and ignored names (res::is_ignored_asset_name, ported below and
checked against tests/data/asset_ignored_names.txt, which a C++ test checks too) are neither listed nor entered, links
are not followed, and a folder that is a git submodule (a .git inside, or a path of .gitmodules) is left out.
"""
import json
import math
import os
import sys
import tempfile
import uuid
from decimal import Decimal
from pathlib import Path

META_SUFFIX = '.resource'
META_TYPE = 'resource_meta'
META_VERSION = 1
META_KEYS = ('__type', '__version', '__guid', 'importer', 'settings', 'flags')
META_MAX_BYTES = 64 * 1024
IGNORED_NAMES_TABLE = Path(__file__).resolve().parents[2] / 'tests' / 'data' / 'asset_ignored_names.txt'

_ASCII_LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def ascii_lower(text):
    # Only A-Z, as the C++ rules: str.lower() would also fold letters of other alphabets.
    return text.translate(_ASCII_LOWER)


def is_asset_meta_name(name):
    """res::is_asset_meta_name."""
    return len(name) >= len(META_SUFFIX) and ascii_lower(name[-len(META_SUFFIX):]) == META_SUFFIX


def is_sidecar_name(name):
    return len(name) > len(META_SUFFIX) and is_asset_meta_name(name)


def is_ignored_asset_name(name):
    """res::is_ignored_asset_name."""
    if not name or name[0] == '.' or name.startswith('~$') or name[-1] == '~':
        return True
    if ascii_lower(name) in ('thumbs.db', 'desktop.ini'):
        return True
    return len(name) > 4 and ascii_lower(name[-4:]) == '.tmp'


# ---- res::write_pretty_json ----

def _double_text(number):
    """std::to_chars(double): the shortest digits that read back, fixed or scientific (whichever is shorter, fixed on
    a tie), and ".0" when neither a '.' nor an exponent is in the text."""
    if not math.isfinite(number):
        raise ValueError(f'{number} has no JSON text')
    sign, digits, exponent = Decimal(repr(number)).normalize().as_tuple()
    if not any(digits):
        digits, exponent = (0,), 0
    text = ''.join(str(d) for d in digits)
    point = exponent + len(digits) - 1  # the exponent of the first digit
    scientific = text[0] + ('.' + text[1:] if len(text) > 1 else '') + ('e+' if point >= 0 else 'e-') + f'{abs(point):02d}'
    if exponent >= 0:
        fixed = text + '0' * exponent
    elif point >= 0:
        fixed = text[:point + 1] + '.' + text[point + 1:]
    else:
        fixed = '0.' + '0' * (-point - 1) + text
    out = ('-' if sign else '') + (fixed if len(fixed) <= len(scientific) else scientific)
    return out if ('.' in out or 'e' in out) else out + '.0'


def _string_text(text):
    # boost::json::serialize: \" \\ \b \f \n \r \t, other control characters as \u00xx; the rest as UTF-8.
    return json.dumps(text, ensure_ascii=False)


def _write_value(out, value, indent):
    if isinstance(value, dict):
        if not value:
            out.append('{}')
        else:
            out.append('{\n')
            inner = indent + '    '
            out.append(',\n'.join(inner + _string_text(key) + ' : ' + _value_text(item, inner) for key, item in value.items()))
            out.append('\n' + indent + '}')
    elif isinstance(value, list):
        text_length = sum(len(item.encode('utf-8')) + 4 for item in value if isinstance(item, str))
        if text_length <= 60 and not any(isinstance(item, (list, dict)) for item in value):
            out.append('[' + ', '.join(_value_text(item, indent + '    ') for item in value) + ']')
        else:
            inner = indent + '    '
            out.append('[\n' + ',\n'.join(inner + _value_text(item, inner) for item in value) + '\n' + indent + ']')
    elif isinstance(value, str):
        out.append(_string_text(value))
    elif isinstance(value, bool):
        out.append('true' if value else 'false')
    elif isinstance(value, int):
        # boost::json reads an integer outside int64 and uint64 as a double.
        out.append(str(value) if -2**63 <= value < 2**64 else _double_text(float(value)))
    elif isinstance(value, float):
        out.append(_double_text(value))
    elif value is None:
        out.append('null')
    else:
        raise ValueError(f'{value!r} is not a JSON value')


def _value_text(value, indent):
    out = []
    _write_value(out, value, indent)
    return ''.join(out)


def pretty_json(value):
    """res::write_pretty_json: four spaces per level, "key" : value, LF line ends and one at the end."""
    return _value_text(value, '') + '\n'


# ---- res::read_asset_meta / res::write_asset_meta ----

def asset_id_from_text(text):
    """res::asset_id_from_text: 8-4-4-4-12 hex digits in any letter case; the lowercase text, or None."""
    if not isinstance(text, str) or len(text) != 36:
        return None
    for i, c in enumerate(text):
        if i in (8, 13, 18, 23):
            if c != '-':
                return None
        elif c not in '0123456789abcdefABCDEF':
            return None
    return text.lower()


def is_nil(id_text):
    return id_text == '00000000-0000-0000-0000-000000000000'


def write_asset_meta(guid, importer='', settings=None, flags=()):
    return pretty_json({
        '__type': META_TYPE,
        '__version': META_VERSION,
        '__guid': guid,
        'importer': importer,
        'settings': settings if settings is not None else {},
        'flags': list(flags),
    }).encode('utf-8')


class MetaError(Exception):
    pass


def _reject_constant(name):
    raise ValueError(f'{name} is not JSON')


class _Pairs(list):
    pass


def _rebuild(value):
    if isinstance(value, _Pairs):
        return {key: _rebuild(item) for key, item in value}
    if isinstance(value, list):
        return [_rebuild(item) for item in value]
    return value


def read_asset_meta(data):
    """The checks of res::read_asset_meta, in its order. Returns (guid, importer, settings, flags); raises MetaError."""
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    try:
        # Pairs, not dicts: the root is checked for a key given twice; below it the last one wins, as in boost::json.
        document = json.loads(data.decode('utf-8'), object_pairs_hook=_Pairs, parse_constant=_reject_constant)
    except (UnicodeDecodeError, ValueError) as error:
        raise MetaError(f'not JSON: {error}')
    if not isinstance(document, _Pairs):
        raise MetaError('the document is not an object')
    keys = [key for key, _ in document]
    for i, key in enumerate(keys):
        if key in keys[:i]:
            raise MetaError(f'{key}: the key is given twice')
    root = {key: _rebuild(item) for key, item in document}

    if '__type' not in root:
        raise MetaError('__type: missing')
    if root['__type'] != META_TYPE:
        raise MetaError(f'__type: expected "{META_TYPE}"')
    if '__version' not in root:
        raise MetaError('__version: missing')
    version = root['__version']
    if isinstance(version, bool) or not isinstance(version, int) or version >= 2**63 or version < 1:
        raise MetaError('__version: expected a positive integer')
    if version > META_VERSION:
        raise MetaError(f'__version: version {version} is newer than this script reads ({META_VERSION})')
    for key in root:
        if key not in META_KEYS:
            raise MetaError(f'{key}: unknown key')
    if '__guid' not in root:
        raise MetaError('__guid: missing')
    guid = asset_id_from_text(root['__guid'])
    if guid is None:
        raise MetaError('__guid: expected a UUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)')
    if is_nil(guid):
        raise MetaError('__guid: the nil UUID is not an asset id')
    if 'importer' not in root:
        raise MetaError('importer: missing')
    if not isinstance(root['importer'], str):
        raise MetaError('importer: expected a string')
    if 'settings' not in root:
        raise MetaError('settings: missing')
    if not isinstance(root['settings'], dict):
        raise MetaError('settings: expected an object')
    if 'flags' not in root:
        raise MetaError('flags: missing')
    if not isinstance(root['flags'], list):
        raise MetaError('flags: expected an array')
    for i, flag in enumerate(root['flags']):
        if not isinstance(flag, str):
            raise MetaError(f'flags[{i}]: expected a string')
    return guid, root['importer'], root['settings'], root['flags']


def desc_root_guid(path):
    """The "__guid" of a .desc's root object when it is an asset ID, else None."""
    if not ascii_lower(path.name).endswith('.desc'):
        return None
    data = path.read_bytes()
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    try:
        document = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        return None
    if not isinstance(document, dict):
        return None
    guid = asset_id_from_text(document.get('__guid'))
    return None if guid is None or is_nil(guid) else guid


# ---- the walk ----

def submodule_paths(tree):
    """The submodule folders (resolved) of the repository that holds `tree`, from its .gitmodules."""
    found = set()
    for folder in [tree.resolve(), *tree.resolve().parents]:
        if (folder / '.git').exists():
            modules = folder / '.gitmodules'
            if modules.is_file():
                for line in modules.read_text(encoding='utf-8', errors='replace').splitlines():
                    name, sep, value = line.strip().partition('=')
                    if sep and name.strip() == 'path':
                        found.add((folder / value.strip()).resolve())
            break
    return found


def walk(tree, submodules):
    """(asset files, sidecars) below `tree`, as paths; both sorted."""
    files = []
    sidecars = []
    pending = [tree]
    while pending:
        folder = pending.pop()
        with os.scandir(folder) as entries:
            for entry in entries:
                if entry.is_symlink():
                    continue
                name = entry.name
                path = Path(entry.path)
                if entry.is_dir(follow_symlinks=False):
                    if is_ignored_asset_name(name) or (path / '.git').exists() or path.resolve() in submodules:
                        continue
                    pending.append(path)
                elif entry.is_file(follow_symlinks=False):
                    if is_sidecar_name(name):
                        if not is_ignored_asset_name(name[:-len(META_SUFFIX)]):
                            sidecars.append(path)
                    elif not is_ignored_asset_name(name):
                        files.append(path)
    return sorted(files), sorted(sidecars)


def pair_up(files, sidecars):
    """{file: sidecar or None}, [orphan sidecars], [(file, [sidecars]) given more than one]. Matched in any letter case."""
    by_key = {}
    for path in files:
        by_key[(path.parent, ascii_lower(path.name))] = path
    owned = {path: [] for path in files}
    orphans = []
    for sidecar in sidecars:
        owner = by_key.get((sidecar.parent, ascii_lower(sidecar.name[:-len(META_SUFFIX)])))
        if owner is None:
            orphans.append(sidecar)
        else:
            owned[owner].append(sidecar)
    pairs = {path: (found[0] if found else None) for path, found in owned.items()}
    several = [(path, found) for path, found in owned.items() if len(found) > 1]
    return pairs, orphans, several


def collect(trees):
    files = []
    sidecars = []
    for tree in trees:
        found_files, found_sidecars = walk(tree, submodule_paths(tree))
        files += found_files
        sidecars += found_sidecars
    return files, sidecars


def held_ids(sidecars):
    ids = set()
    for sidecar in sidecars:
        try:
            data = sidecar.read_bytes()
            if len(data) <= META_MAX_BYTES:
                ids.add(read_asset_meta(data)[0])
        except (OSError, MetaError):
            pass
    return ids


def write(trees, new_id=lambda: str(uuid.uuid4())):
    """Makes the missing sidecars; returns their paths."""
    files, sidecars = collect(trees)
    pairs, _, _ = pair_up(files, sidecars)
    taken = held_ids(sidecars)
    created = []
    for path in sorted(p for p, sidecar in pairs.items() if sidecar is None):
        guid = desc_root_guid(path)
        if guid is None or guid in taken:
            guid = new_id()
            while guid in taken:
                guid = new_id()
        taken.add(guid)
        sidecar = path.with_name(path.name + META_SUFFIX)
        # 'x': a sidecar that appeared meanwhile is the file's ID, never replaced.
        with open(sidecar, 'xb') as out:
            out.write(write_asset_meta(guid))
        created.append(sidecar)
    return created


def check(trees):
    """One line per problem."""
    files, sidecars = collect(trees)
    pairs, orphans, several = pair_up(files, sidecars)
    errors = []
    for path, sidecar in sorted(pairs.items()):
        if sidecar is None:
            errors.append(f'{path}: no {META_SUFFIX} (run tools/ci/asset_ids.py --write on its tree and commit it)')
    for sidecar in orphans:
        errors.append(f'{sidecar}: no file {sidecar.name[:-len(META_SUFFIX)]} next to it (remove it or restore the file)')
    for path, found in several:
        errors.append(f'{path}: {len(found)} {META_SUFFIX} files in different letter case: ' + ', '.join(p.name for p in found))
    owners = {sidecar: path for path, sidecar in pairs.items() if sidecar is not None}
    holders = {}
    for sidecar in sidecars:
        data = sidecar.read_bytes()
        if len(data) > META_MAX_BYTES:
            errors.append(f'{sidecar}: {len(data)} bytes, more than the {META_MAX_BYTES} the editor reads')
            continue
        try:
            guid, importer, settings, flags = read_asset_meta(data)
        except MetaError as error:
            errors.append(f'{sidecar}: {error}')
            continue
        holders.setdefault(guid, []).append(sidecar)
        try:
            canonical = write_asset_meta(guid, importer, settings, flags)
        except ValueError:
            canonical = None
        if canonical != data:
            errors.append(f'{sidecar}: not in the form res::write_asset_meta writes (key order, 4-space indent, LF line ends, '
                          f'lowercase ID, a newline at the end)')
        owner = owners.get(sidecar)
        if owner is not None:
            root_guid = desc_root_guid(owner)
            if root_guid is not None and root_guid != guid:
                errors.append(f'{sidecar}: holds {guid}, but its desc has "__guid" {root_guid}')
    for guid, found in sorted(holders.items()):
        if len(found) > 1:
            errors.append(f'{guid} is the ID of {len(found)} files: ' + ', '.join(str(p) for p in found))
    return errors


# ---- self-test ----

def expect(condition, what):
    # Not assert: python -O would skip every check.
    if not condition:
        raise SystemExit(f'asset_ids: self-test failed: {what}')


def read_ignored_names_table(path=IGNORED_NAMES_TABLE):
    """[(name, ignored)] of the table the C++ test reads too: "<name>\\t<0|1>" per line, '#' lines are comments."""
    rows = []
    for number, line in enumerate(path.read_bytes().decode('utf-8').split('\n'), 1):
        line = line.rstrip('\r')
        if not line or line.startswith('#'):
            continue
        name, sep, flag = line.rpartition('\t')
        expect(sep and flag in ('0', '1'), f'{path}:{number}: expected "<name>\\t<0|1>", got {line!r}')
        rows.append((name, flag == '1'))
    return rows


BARE_META = ('{\n'
             '    "__type" : "resource_meta",\n'
             '    "__version" : 1,\n'
             '    "__guid" : "00000000-0000-4000-8000-000000000001",\n'
             '    "importer" : "",\n'
             '    "settings" : {},\n'
             '    "flags" : []\n'
             '}\n').encode()

# tests/res_asset_meta_tests.cpp, full_meta: what res::write_asset_meta writes for these values.
FULL_META = ('{\n'
             '    "__type" : "resource_meta",\n'
             '    "__version" : 1,\n'
             '    "__guid" : "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f",\n'
             '    "importer" : "texture",\n'
             '    "settings" : {\n'
             '        "srgb" : true,\n'
             '        "mips" : {\n'
             '            "count" : 4,\n'
             '            "filter" : "box"\n'
             '        },\n'
             '        "scale" : 0.5\n'
             '    },\n'
             '    "flags" : ["no_pack", "editor_only"]\n'
             '}\n').encode()


def _self_test_names():
    rows = read_ignored_names_table()
    expect(len(rows) >= 20, f'{IGNORED_NAMES_TABLE} holds only {len(rows)} names')
    for name, ignored in rows:
        expect(is_ignored_asset_name(name) == ignored, f'is_ignored_asset_name({name!r}) is not {ignored}')
    expect(is_asset_meta_name('a.png.RESOURCE') and is_asset_meta_name('.resource'), 'a sidecar name in any case')
    expect(not is_sidecar_name('.resource') and not is_asset_meta_name('a.resources'), 'a name that is no sidecar')


def _self_test_format():
    expect(write_asset_meta('00000000-0000-4000-8000-000000000001') == BARE_META, 'a bare sidecar is not the C++ text')
    meta = read_asset_meta(FULL_META)
    expect(write_asset_meta(*meta) == FULL_META, 'full_meta does not round-trip')
    expect(read_asset_meta(b'\xef\xbb\xbf' + FULL_META) == meta, 'a BOM is not skipped')
    for value, text in ((0.5, '0.5'), (1.0, '1.0'), (100.0, '100.0'), (1e16, '1e+16'), (1e-05, '1e-05'),
                        (0.001, '0.001'), (-2.5, '-2.5'), (0.0, '0.0'), (123456.789, '123456.789'), (1.5e300, '1.5e+300')):
        expect(_double_text(value) == text, f'{value} is written as {_double_text(value)}, not {text}')
    expect(pretty_json({'a': [{'b': 1}], 'c': ['x' * 60]}) ==
           '{\n    "a" : [\n        {\n            "b" : 1\n        }\n    ],\n    "c" : [\n        "' + 'x' * 60 + '"\n    ]\n}\n',
           'nested and long arrays are not one element per line')
    expect(pretty_json({'s': 'a"b\\c\n\x01/'}) == '{\n    "s" : "a\\"b\\\\c\\n\\u0001/"\n}\n', 'string escapes')

    def refused(text, field):
        try:
            read_asset_meta(text.encode() if isinstance(text, str) else text)
        except MetaError as error:
            expect(str(error).startswith(field), f'the refusal names {error}, not {field}')
            return
        raise SystemExit(f'asset_ids: self-test failed: {text!r} is read')

    full = FULL_META.decode()
    refused(full.replace('3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f', 'not-a-guid'), '__guid')
    refused(full.replace('3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f', '00000000-0000-0000-0000-000000000000'), '__guid')
    refused(full.replace('"__version" : 1', '"__version" : 1.0'), '__version')
    refused(full.replace('"__version" : 1', '"__version" : true'), '__version')
    refused(full.replace('"__version" : 1', '"__version" : 2'), '__version')
    refused(full.replace('"resource_meta"', '"material"'), '__type')
    refused(full.replace('"importer" : "texture"', '"importer" : 3'), 'importer')
    refused(BARE_META.decode().replace('"settings" : {}', '"settings" : []'), 'settings')
    refused(full.replace('["no_pack", "editor_only"]', '["no_pack", 7]'), 'flags[1]')
    refused(full.replace('    "flags"', '    "extra" : null,\n    "flags"'), 'extra')
    refused(full.replace('    "importer"', '    "__guid" : "00000000-0000-4000-8000-000000000001",\n    "importer"'), '__guid')
    refused('[1, 2]', '')
    refused('{"__type" : "resource_meta", "x" : NaN}', 'not JSON')
    # Equal keys below the root are the importer's data: read, the last one wins.
    expect(read_asset_meta(full.replace('"scale" : 0.5', '"scale" : 0.5, "srgb" : false').encode())[2]['srgb'] is False,
           'equal keys in settings')


def _self_test_trees():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / '.git').mkdir()
        (root / '.gitmodules').write_text('[submodule "s"]\n\tpath = tree/samples\n\turl = x\n')
        tree = root / 'tree'
        for relative, text in (('a.png', 'x'), ('levels/main.desc', '{"__guid" : "7C3E9A51-2B84-4D0F-9E6A-1F5B8D2C4A90"}'),
                               ('levels/copy.desc', '{"__guid" : "7c3e9a51-2b84-4d0f-9e6a-1f5b8d2c4a90"}'),
                               ('levels/nil.desc', '{"__guid" : "00000000-0000-0000-0000-000000000000"}'),
                               ('levels/list.desc', '[]'), ('.hidden/b.png', 'x'), ('Thumbs.db', 'x'), ('x.tmp', 'x'),
                               ('x.tmp.resource', 'x'), ('samples/m.glb', 'x'), ('module/.git', 'x'), ('module/c.png', 'x'),
                               ('Upper.PNG', 'x'), ('upper.png.Resource', BARE_META.decode())):
            path = tree / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, newline='\n')
        if hasattr(os, 'symlink'):
            try:
                os.symlink(tree / 'a.png', tree / 'link.png')
                os.symlink(tree / 'levels', tree / 'linked_levels', target_is_directory=True)
            except OSError:
                pass

        ids = iter(f'00000000-0000-4000-8000-00000000000{n}' for n in range(2, 10))
        created = write([tree], new_id=lambda: next(ids))
        names = sorted(p.relative_to(tree).as_posix() for p in created)
        expect(names == ['a.png.resource', 'levels/copy.desc.resource', 'levels/list.desc.resource',
                         'levels/main.desc.resource', 'levels/nil.desc.resource'], f'--write made {names}')
        main_id = read_asset_meta((tree / 'levels/main.desc.resource').read_bytes())[0]
        copy_id = read_asset_meta((tree / 'levels/copy.desc.resource').read_bytes())[0]
        expect(main_id == '7c3e9a51-2b84-4d0f-9e6a-1f5b8d2c4a90' or copy_id == '7c3e9a51-2b84-4d0f-9e6a-1f5b8d2c4a90',
               'a desc does not keep its __guid')
        expect(main_id != copy_id, 'two descs with one __guid got one ID')
        expect(write([tree]) == [], 'a second --write changes something')

        # Both descs claim the __guid; the one that lost it is reported, as the editor's Problems would.
        errors = check([tree])
        expect(len(errors) == 1 and 'main.desc.resource: holds' in errors[0], f'--check on the written tree gives {errors}')
        (tree / 'levels/main.desc').write_text('{}')
        expect(check([tree]) == [], f'--check on a clean tree gives {check([tree])}')

        (tree / 'a.png').unlink()
        (tree / 'd.png').write_text('x')
        (tree / 'levels/list.desc.resource').write_bytes((tree / 'levels/list.desc.resource').read_bytes().replace(b'\n', b'\r\n'))
        (tree / 'levels/nil.desc.resource').write_bytes(BARE_META)
        errors = check([tree])
        expect(len(errors) == 4, f'--check on a broken tree gives {errors}')
        expect(any('d.png: no .resource' in e for e in errors), 'a file without a sidecar')
        expect(any('a.png.resource: no file a.png' in e for e in errors), 'an orphan sidecar')
        expect(any('list.desc.resource: not in the form' in e for e in errors), 'CRLF line ends')
        expect(sum('00000000-0000-4000-8000-000000000001 is the ID of 2 files' in e for e in errors) == 1, 'a duplicate ID')


def self_test():
    _self_test_names()
    _self_test_format()
    _self_test_trees()
    print('asset_ids: self-test passed')


def main(argv):
    if argv == ['--self-test']:
        self_test()
        return 0
    if len(argv) < 2 or argv[0] not in ('--write', '--check'):
        print(__doc__, file=sys.stderr)
        return 2
    trees = [Path(tree) for tree in argv[1:]]
    for tree in trees:
        if not tree.is_dir():
            print(f'asset_ids: {tree} is not a directory (run from the repository root)')
            return 2
    if argv[0] == '--write':
        created = write(trees)
        for path in created:
            print(f'created {path}')
        print(f'asset_ids: {len(created)} .resource file(s) created')
        return 0
    errors = check(trees)
    for error in errors:
        print(error)
    if errors:
        print(f'asset_ids: {len(errors)} problem(s)')
        return 1
    print('asset_ids: every asset file of the trees has its .resource')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
