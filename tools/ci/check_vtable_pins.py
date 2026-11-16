#!/usr/bin/env python3
"""Fails when the base classes or virtual functions of an engine interface a module calls or fills change and its ABI
revision does not (#922).

    check_vtable_pins.py [--pins <file>] [--repo <root>] [--base-pins <file>]   check
    check_vtable_pins.py --update [--pins <file>] [--repo <root>]              record the current lists and revisions
    (defaults: --pins tools/ci/vtable_pins.json, --repo .)
    check_vtable_pins.py --self-test

A game module built against an engine fills or calls the vtable of these interfaces inline, by slot. The layout key
(gs::module_layout_key, snk/game/gs_module_layout_key.h) sees no vtable: it mixes in the <interface>_abi_revision
constant declared next to each interface instead. A game_editor module does the same with the editor's interfaces,
whose constants the editor layout key mixes in (edt::editor_layout_key, snk/editor/edt_editor_layout_key.h, #969): their
pins carry "key": "editor"; a pin without "key" is the game's, any other value is an error. Each key has its revision:
layout_revision (gs_module_layout.h) and editor_layout_revision (edt_editor_layout.h), recorded under the same names at
the top of the pin file. The pin file holds, per interface, its header, the constant, the key, the
revision, the ordered list of its base classes and the ordered list of the virtual functions the class declares. The
check fails when:
  - the bases or the virtual functions in the header differ from the pinned ones while the constant still has the pinned
    revision: raise the constant by one (and its key's revision with it, see the rule above SNK_MODULE_API_VERSION) and
    record the new lists with --update;
  - the constant differs from the pinned revision: record it with --update;
  - the header, the class or the constant is gone, or its key header does not name the constant;
  - a key's revision in its layout header differs from the one the pin file records: record it with --update;
  - a constant listed in "retired" is also an active pin, or a pin names an unknown key;
  - with --base-pins (the pin file of the base branch, a path relative to the current directory):
      - an interface pinned in both files has other bases or virtual functions in each and a revision that is not
        strictly higher in this branch, or its key's revision is not strictly higher than the base's. This catches a
        pin file edited by hand to match changed headers;
      - a pin of the base branch (matched by its constant) is missing here. Restore it, or retire it: add
        {"constant": "<name>_abi_revision", "reason": "<one line>"} to the top-level "retired" list and raise
        its key's revision. A retired entry stays in the file afterwards. Renaming a constant is a removal plus a new pin;
      - a pin was retired relative to the base and its key's revision is not higher than the base's. A pin whose "key"
        differs from the base pin of the same constant counts as retired from the old key (that key's revision must be
        higher than the base's) and as a new pin in its new key. A base file without "layout_revision" (older
        format) has an unknown game revision: the change passes with a note; one without "editor_layout_revision"
        reads it as 0.
    A pin only in this branch is new and passes. A missing or malformed --base-pins file is an error: the caller leaves
    the flag out when the base has none.
--update refuses the first case too: it records changed lists only with a raised revision. It keeps "retired".

What is pinned: the base specifiers after the class name, in order, as `[access] [virtual] <name as spelled>` (a new,
removed or reordered base or another one moves the slots of a vtable), and each declaration in the class body (not in a
nested class) that contains the word `virtual`, in order, as `<return type> <name>(<parameter types>) <qualifiers>`:
parameter names, default arguments, `virtual`, `override`, `final`, `= 0`, `= default`, an inline body and
[[attributes]] are dropped, spaces normalized. So a reorder, a new or a removed virtual, another parameter or return
type, const, &/&& or noexcept count as a change; a renamed parameter, a comment or a new body do not.

It is a text parser, not a compiler. It does not see:
  - a virtual function brought in by a macro, or a class declared by one;
  - an override written without `virtual` (it adds no slot to a single-inheritance vtable; a covariant return can);
  - a change of a typedef or alias a signature or a base names (the spelling is compared, not the type), nor a change
    inside a base that is not pinned itself;
  - preprocessor branches: every #if branch is read as if it were taken;
  - a class whose head between its name and `{` holds parentheses (alignas, decltype in a base).
A parameter that is a function pointer or an array keeps its name in the pinned text.
"""
import json
import re
import sys
from pathlib import Path

DEFAULT_PINS = 'tools/ci/vtable_pins.json'
KEY_HEADER = 'core/engine/game_system/include/snk/game/gs_module_layout_key.h'
LAYOUT_HEADER = 'core/engine/game_system/include/snk/game/gs_module_layout.h'
EDITOR_KEY_HEADER = 'Editor/code/editor_system/include/snk/editor/edt_editor_layout_key.h'
EDITOR_LAYOUT_HEADER = 'Editor/code/editor_system/include/snk/editor/edt_editor_layout.h'
TOOL = 'check_vtable_pins'

# The keys a pin can belong to (its "key", "game" when absent): where its revision constant must be mixed in, and the
# revision of that key, a constant of its layout header recorded under the same name at the top of the pin file.
# `base_default` is what a base pin file without that revision reads as: None = unknown (a note), a number = that.
KEYS = {
    'game': {'key_header': KEY_HEADER, 'layout_header': LAYOUT_HEADER, 'revision': 'layout_revision',
             'list': 'interface_abi_revisions', 'base_default': None},
    'editor': {'key_header': EDITOR_KEY_HEADER, 'layout_header': EDITOR_LAYOUT_HEADER,
               'revision': 'editor_layout_revision', 'list': 'editor_interface_abi_revisions', 'base_default': 0},
}

BUILTIN_WORDS = {'bool', 'char', 'char8_t', 'char16_t', 'char32_t', 'wchar_t', 'short', 'int', 'long', 'signed',
                 'unsigned', 'float', 'double', 'void', 'auto', 'const', 'volatile'}
IDENT = re.compile(r'[A-Za-z_]\w*')
SINGLE_COLON = re.compile(r'(?<!:):(?!:)')


def strip_comments(text):
    """Comments and the contents of string and character literals go; newlines stay, so nothing else moves."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            # A ' after a digit or letter is a digit separator (0x0004'0000u), not a character literal.
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def skip_preprocessor(text):
    lines = text.split('\n')
    continued = False
    for index, line in enumerate(lines):
        if continued or line.lstrip().startswith('#'):
            continued = line.rstrip().endswith('\\')
            lines[index] = ''
    return '\n'.join(lines)


def find_matching(text, start, open_char, close_char):
    """Index of the bracket closing the one at `start`."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return i
    raise ValueError(f'unbalanced {open_char}{close_char}')


def class_definition(text, name):
    """(base specifiers, body) of the definition of class/struct `name`, or None."""
    pattern = re.compile(r'\b(?:class|struct)\s+(?:\w+\s+)*?' + re.escape(name) + r'\b')
    for match in pattern.finditer(text):
        rest = match.end()
        # A definition reaches `{` before `;` (a forward declaration or a friend reaches `;`).
        brace = text.find('{', rest)
        semicolon = text.find(';', rest)
        if brace < 0 or (0 <= semicolon < brace):
            continue
        between = text[rest:brace]
        if '(' in between or ')' in between:
            continue
        return base_specifiers(between), text[brace + 1:find_matching(text, brace, '{', '}')]
    return None


def base_specifiers(head):
    """The base classes after the class name, in order, as `[access] [virtual] <name as spelled>`."""
    colon = SINGLE_COLON.search(head)
    if colon is None:
        return []
    bases = []
    for part in split_top_level(head[colon.end():]):
        words = normalize_spaces(part)
        if not words:
            continue
        # `virtual public b` and `public virtual b` are one base.
        access = re.search(r'\b(public|protected|private)\b', words)
        is_virtual = re.search(r'\bvirtual\b', words)
        name = normalize_spaces(re.sub(r'\b(?:public|protected|private|virtual)\b', ' ', words))
        bases.append(' '.join(w for w in (access and access.group(1), is_virtual and 'virtual', name) if w))
    return bases


def member_declarations(body):
    """The declarations of the class body, at its own level, each up to its `;` or the `{` of its inline body."""
    declarations = []
    current = []
    depth_paren = 0
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c in '([':
            depth_paren += 1
        elif c in ')]':
            depth_paren -= 1
        if c == '{' and depth_paren == 0:
            end = find_matching(body, i, '{', '}')
            head = ''.join(current)
            if '(' in head and in_member_initializers(head):
                # `x() : m{1} {}`: the brace of a member initializer, the body comes after it.
                current.append(body[i:end + 1])
            elif '(' in head:
                # An inline function body ends the declaration.
                declarations.append(head)
                current = []
            elif re.search(r'\b(?:class|struct|union|enum)\b', head):
                # A nested type: neither its members nor itself are declarations of this class.
                current = []
            else:
                # A brace initializer of a data member: part of its declaration.
                current.append(body[i:end + 1])
            i = end + 1
            continue
        if c == ';' and depth_paren == 0:
            declarations.append(''.join(current))
            current = []
        else:
            current.append(c)
        i += 1
    return [d for d in (d.strip() for d in declarations) if d]


def in_member_initializers(head):
    """Whether a `{` after `head` (a declaration with a parameter list) opens a member initializer of a constructor:
    a single `:` follows the parameter list and the brace follows a name or a `>`."""
    open_paren = head.index('(')
    after = head[find_matching(head, open_paren, '(', ')') + 1:]
    return SINGLE_COLON.search(after) is not None and re.search(r'[\w>]\s*$', head) is not None


CLOSING = {')': '(', ']': '[', '}': '{', '>': '<'}


def split_top_level(text, separator=','):
    """`text` split at `separator` outside brackets. `<` and `>` are angle brackets only outside (), [] and {}: the `<`
    of `int a = (1 < 2)` is a comparison."""
    parts = []
    stack = []
    current = []
    for c in text:
        if c in '([{' or (c == '<' and (not stack or stack[-1] == '<')):
            stack.append(c)
        elif c in ')]}' or (c == '>' and stack and stack[-1] == '<'):
            # A `)` also closes a `<` left open inside its parentheses.
            while stack and stack[-1] != CLOSING[c]:
                stack.pop()
            if stack:
                stack.pop()
        if c == separator and not stack:
            parts.append(''.join(current))
            current = []
        else:
            current.append(c)
    parts.append(''.join(current))
    return parts


TOKEN = re.compile(r'(\s*)(::|&&|[A-Za-z_]\w*|\d\w*|\.\.\.|\S)')


def normalize_spaces(text):
    """One space where the source had some between two words, or between `>`, `*`, `&` or a word and a name or `::`
    (`const ::res::tag&`, `std::vector<T> name`); none anywhere else."""
    out = []
    previous = None
    for match in TOKEN.finditer(text):
        space, token = match.groups()
        if previous is not None and space:
            left_word = IDENT.fullmatch(previous) or previous[0].isdigit() or previous in ('>', '*', '&', '&&')
            right_word = IDENT.fullmatch(token) or token[0].isdigit() or token == '::'
            if left_word and right_word:
                out.append(' ')
        if token == ',':
            out.append(', ')
            previous = None
            continue
        out.append(token)
        previous = token
    return ''.join(out).strip()


def parameter_type(parameter):
    parameter = split_top_level(parameter, '=')[0].strip()
    if not parameter or parameter == 'void':
        return parameter
    tokens = re.findall(r'::|[A-Za-z_]\w*|\.\.\.|\S', parameter)
    if len(tokens) >= 2 and IDENT.fullmatch(tokens[-1]) and tokens[-1] not in BUILTIN_WORDS and tokens[-2] != '::':
        # The last word names the parameter unless the type is a single word (`T`) or ends in a keyword.
        before = tokens[-2]
        only_cv = all(token in ('const', 'volatile') for token in tokens[:-1])
        if not only_cv and (IDENT.fullmatch(before) or before in ('*', '&', '>', '...')):
            cut = parameter.rfind(tokens[-1])
            parameter = parameter[:cut]
    return normalize_spaces(parameter)


def signature(declaration):
    """The pinned text of one virtual function declaration."""
    text = re.sub(r'\[\[.*?\]\]', ' ', declaration)
    text = re.sub(r'^(?:\s*(?:public|protected|private)\s*:)+', ' ', text)
    open_paren = text.index('(')
    close_paren = find_matching(text, open_paren, '(', ')')
    head = text[:open_paren]
    parameters = text[open_paren + 1:close_paren]
    tail = text[close_paren + 1:]
    head = re.sub(r'\b(?:virtual|inline|SNK_API|SNK_EDITOR_API)\b', ' ', head)
    tail = re.sub(r'=\s*(?:0|default|delete)\s*$', ' ', tail.strip())
    tail = re.sub(r'\b(?:override|final)\b', ' ', tail)
    types = [parameter_type(p) for p in split_top_level(parameters)] if parameters.strip() else []
    head = normalize_spaces(head)
    tail = normalize_spaces(tail)
    text = f"{head}({', '.join(t for t in types if t)})"
    return f'{text} {tail}'.strip() if tail else text


def pin_of(header_text, class_name):
    """(base specifiers, ordered signatures of the virtual functions) `class_name` declares, or None when it is not
    defined."""
    found = class_definition(skip_preprocessor(strip_comments(header_text)), class_name)
    if found is None:
        return None
    bases, body = found
    return bases, [signature(d) for d in member_declarations(body) if re.search(r'\bvirtual\b', d) and '(' in d]


def virtuals_of(header_text, class_name):
    pin = pin_of(header_text, class_name)
    return None if pin is None else pin[1]


def revision_of(header_text, constant):
    match = re.search(r'\b' + re.escape(constant) + r'\s*=\s*(\d+)\s*;', strip_comments(header_text))
    return int(match.group(1)) if match else None


def key_of(entry):
    """The key a pin belongs to, or None when its "key" names none."""
    key = entry.get('key', 'game')
    return key if key in KEYS else None


def key_problems(pins):
    return [f"{DEFAULT_PINS}: {entry['class']} ({entry['constant']}) has \"key\": {json.dumps(entry.get('key'))}: "
            f"expected one of {', '.join(json.dumps(k) for k in KEYS)} (none = \"game\")"
            for entry in pins['interfaces'] if key_of(entry) is None]


def layout_revision_of(read, key='game'):
    """The revision constant of the key's layout header, or None."""
    text = read(KEYS[key]['layout_header'])
    return None if text is None else revision_of(text, KEYS[key]['revision'])


def no_revision_error(key):
    return f"{KEYS[key]['layout_header']}: no `inline constexpr std::uint64_t {KEYS[key]['revision']} = <n>;`"


def retired_constants(pins):
    return {entry['constant'] for entry in pins.get('retired', [])}


def retired_problems(pins):
    """A retired constant that is also an active pin: the entry would hide a pin that is still checked."""
    active = {entry['constant'] for entry in pins['interfaces']}
    return [f'{DEFAULT_PINS}: {constant} is in "retired" and also pinned: drop one of them'
            for constant in sorted(retired_constants(pins) & active)]


def current_state(entry, read):
    """(pin, revision, problems) of one pinned interface, the pin as {'bases': [...], 'virtuals': [...]}; `read` maps a
    repository path to its text or None."""
    text = read(entry['header'])
    if text is None:
        return None, None, [f"{entry['header']}: not found (pinned interface {entry['class']})"]
    problems = []
    found = pin_of(text, entry['class'])
    if found is None:
        problems.append(f"{entry['header']}: no definition of class {entry['class']}")
    revision = revision_of(text, entry['constant'])
    if revision is None:
        problems.append(f"{entry['header']}: no `inline constexpr std::uint32_t {entry['constant']} = <n>;`")
    pin = None if found is None else {'bases': found[0], 'virtuals': found[1]}
    return pin, revision, problems


def pinned_of(entry):
    return {'bases': entry.get('bases'), 'virtuals': entry.get('virtuals')}


def differs(before, after):
    """Whether two pins differ. The bases are compared only when both pins hold them."""
    if before['virtuals'] != after['virtuals']:
        return True
    return before['bases'] is not None and after['bases'] is not None and before['bases'] != after['bases']


def describe_change(before, after, labels=('pinned', 'now')):
    lines = []
    for key in ('bases', 'virtuals'):
        old, new = before.get(key), after.get(key)
        if old is None or new is None or old == new:
            continue
        lines += [f'    {key}, {labels[0]}:'] + [f'      {s}' for s in old]
        lines += [f'    {key}, {labels[1]}:'] + [f'      {s}' for s in new]
    return '\n'.join(lines)


def raise_hint(constant, revision, key='game'):
    return (f'A module built before calls them by slot: raise {constant} to {revision + 1} and {KEYS[key]["revision"]} '
            f'in {Path(KEYS[key]["layout_header"]).name}, then run python3 tools/ci/check_vtable_pins.py --update.')


def check(pins, read):
    """The problems of the pin file against the headers of the same tree."""
    errors = retired_problems(pins) + key_problems(pins)
    for key, spec in KEYS.items():
        current = layout_revision_of(read, key)
        if current is None:
            errors.append(no_revision_error(key))
        elif pins.get(spec['revision']) != current:
            errors.append(f'{spec["revision"]} is {current}, the pin file says {pins.get(spec["revision"])}: run '
                          f'python3 tools/ci/check_vtable_pins.py --update and commit {DEFAULT_PINS}')
    key_texts = {key: read(spec['key_header']) for key, spec in KEYS.items()}
    for entry in pins['interfaces']:
        key = key_of(entry)
        if key is None:
            continue
        pin, revision, problems = current_state(entry, read)
        errors += problems
        key_text = key_texts[key]
        if key_text is not None and not re.search(r'\b' + re.escape(entry['constant']) + r'\b', strip_comments(key_text)):
            errors.append(f"{KEYS[key]['key_header']}: does not mix {entry['constant']} into the key ({KEYS[key]['list']})")
        if problems:
            continue
        pinned = pinned_of(entry)
        pinned_revision = entry.get('revision')
        where = f"{entry['header']}: {entry['class']}"
        if pinned['virtuals'] is None or pinned['bases'] is None:
            errors.append(f'{where}: not pinned yet: run python3 tools/ci/check_vtable_pins.py --update')
        elif differs(pinned, pin) and revision == pinned_revision:
            errors.append(f'{where}: its base classes or virtual functions changed but {entry["constant"]} is still '
                          f'{revision}. {raise_hint(entry["constant"], revision, key)}\n' + describe_change(pinned, pin))
        elif revision != pinned_revision:
            errors.append(f'{where}: {entry["constant"]} is {revision}, the pin file says {pinned_revision}: run '
                          f'python3 tools/ci/check_vtable_pins.py --update and commit {DEFAULT_PINS}')
    return errors


def check_against_base(pins, base_pins):
    """(errors, notes) of the pin file against the base branch's: a pin edited by hand to match changed headers passes
    check() but changes here without a higher revision, and a pin cannot vanish without being retired."""
    errors = key_problems(pins)
    notes = []
    base = {entry['constant']: entry for entry in base_pins['interfaces']}
    head = {entry['constant']: entry for entry in pins['interfaces']}

    def raised(key):
        """(raised, here, base) of the key's revision: unknown in the base (None) counts as raised, with a note."""
        name = KEYS[key]['revision']
        base_value = base_pins.get(name, KEYS[key]['base_default'])
        value = pins.get(name)
        if not isinstance(base_value, int):
            return None, value, base_value
        return isinstance(value, int) and value > base_value, value, base_value

    changed_unknown = []
    removed = {}
    for constant, entry in head.items():
        before = base.get(constant)
        key = key_of(entry)
        if key is not None and before is not None and key_of(before) not in (None, key):
            # Another key: a retirement from the old key (its revision must rise) and a new pin in this one.
            removed.setdefault(key_of(before), []).append(f'{constant} (moved to the {key} key)')
            continue
        if key is None or before is None or before.get('virtuals') is None or entry.get('virtuals') is None:
            continue
        if not differs(pinned_of(before), pinned_of(entry)):
            continue
        revision = entry.get('revision')
        base_revision = before.get('revision')
        change = describe_change(pinned_of(before), pinned_of(entry), ('base branch', 'this branch'))
        if not (isinstance(revision, int) and isinstance(base_revision, int) and revision > base_revision):
            errors.append(f"{entry['header']}: {entry['class']}: its pinned base classes or virtual functions differ "
                          f"from the base branch's pin file but {constant} is {revision} here and {base_revision} "
                          f"there, not higher. {raise_hint(constant, base_revision or 0, key)}\n" + change)
            continue
        ok, value, base_value = raised(key)
        name = KEYS[key]['revision']
        if ok is None:
            changed_unknown.append((constant, name))
        elif not ok:
            errors.append(f"{entry['header']}: {entry['class']}: its pinned base classes or virtual functions differ "
                          f"from the base branch's pin file but {name} is {value} here and {base_value} there, not "
                          f"higher: raise it in {Path(KEYS[key]['layout_header']).name} and run python3 "
                          f"tools/ci/check_vtable_pins.py --update\n" + change)
    for constant, name in changed_unknown:
        notes.append(f"{TOOL}: note: the base branch's pin file records no {name}: the change of {constant} is "
                     f"accepted without comparing it")
    retired = retired_constants(pins)
    for constant, before in base.items():
        if constant in head:
            continue
        key = key_of(before) or 'game'
        if constant in retired:
            removed.setdefault(key, []).append(constant)
        else:
            errors.append(f"{before['header']}: {before['class']} ({constant}) is pinned in the base branch but not "
                          f"here: its vtable would no longer be checked. Restore the pin, or add "
                          f'{{"constant": "{constant}", "reason": "..."}} to "retired" in {DEFAULT_PINS} and raise '
                          f'{KEYS[key]["revision"]} in {Path(KEYS[key]["layout_header"]).name}, then run python3 '
                          f'tools/ci/check_vtable_pins.py --update')
    for key, constants in removed.items():
        ok, value, base_value = raised(key)
        name = KEYS[key]['revision']
        if ok is None:
            notes.append(f"{TOOL}: note: the base branch's pin file records no {name}: the retirement of "
                         f"{', '.join(constants)} is accepted without comparing it")
        elif not ok:
            errors.append(f"{', '.join(constants)} retired but {name} is {value} here and {base_value} in the "
                          f"base branch's pin file: raise it in {Path(KEYS[key]['layout_header']).name} and run "
                          f"python3 tools/ci/check_vtable_pins.py --update")
    return errors, notes


def update(pins, read):
    """The pins with the current lists and revisions, or the errors that forbid recording them."""
    errors = retired_problems(pins) + key_problems(pins)
    updated = []
    layouts = {}
    for key, spec in KEYS.items():
        layouts[spec['revision']] = layout_revision_of(read, key)
        if layouts[spec['revision']] is None:
            errors.append(no_revision_error(key))
    for entry in pins['interfaces']:
        if key_of(entry) is None:
            continue
        pin, revision, problems = current_state(entry, read)
        errors += problems
        if problems:
            continue
        pinned = pinned_of(entry)
        if pinned['virtuals'] is not None and differs(pinned, pin) and revision <= entry.get('revision', 0):
            errors.append(f"{entry['header']}: {entry['class']}: its base classes or virtual functions changed: raise "
                          f"{entry['constant']} above {entry.get('revision')} before recording them\n"
                          + describe_change(pinned, pin))
            continue
        recorded_entry = {'class': entry['class'], 'header': entry['header'], 'constant': entry['constant']}
        if 'key' in entry:
            recorded_entry['key'] = entry['key']
        recorded_entry.update({'revision': revision, 'bases': pin['bases'], 'virtuals': pin['virtuals']})
        updated.append(recorded_entry)
    recorded = dict(layouts, interfaces=updated, retired=pins.get('retired', []))
    return (recorded if not errors else None), errors


def load_pins(path):
    """The pin file at `path`, or SystemExit naming what is wrong with it: a malformed file never passes as no pins."""
    try:
        pins = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise SystemExit(f'{TOOL}: cannot read the pin file {path}: {error}')
    entries = pins.get('interfaces') if isinstance(pins, dict) else None
    named = ('class', 'header', 'constant')
    if not isinstance(entries, list) or not all(
            isinstance(e, dict) and all(isinstance(e.get(k), str) for k in named) for e in entries):
        raise SystemExit(f'{TOOL}: {path} is not a pin file: expected {{"interfaces": [{{"class", "header", '
                         f'"constant", ...}}]}}')
    retired = pins.get('retired', [])
    if not isinstance(retired, list) or not all(
            isinstance(e, dict) and isinstance(e.get('constant'), str) and isinstance(e.get('reason'), str)
            and e['reason'].strip() for e in retired):
        raise SystemExit(f'{TOOL}: {path}: "retired" is a list of {{"constant", "reason"}} entries, reason not empty')
    for spec in KEYS.values():
        if spec['revision'] in pins and not isinstance(pins[spec['revision']], int):
            raise SystemExit(f'{TOOL}: {path}: "{spec["revision"]}" is an integer')
    return pins


def expect(condition, what):
    # Not assert: python -O would skip every check.
    if not condition:
        raise SystemExit(f'{TOOL}: self-test failed: {what}')


def self_test():
    header = """#pragma once
#include <cstdint>
namespace demo {
	class iface;
	// iface: virtual int commented_out() = 0;
	inline constexpr std::uint32_t iface_abi_revision = 1;
	class SNK_API iface final : public base
	{
	public:
		virtual ~iface() = default;
		[[nodiscard]] virtual entt::id_type hash() const = 0;
		/* virtual void hidden(); */
		virtual std::unique_ptr<slot> make(const state* state,
			std::span<const participant> contexts, int = 0) const = 0;
		virtual void bind(entt::registry& registry, std::string_view name = "x;{") const override;
		virtual std::vector<std::byte> items(const ::res::tag& tag, const std::vector<std::byte>& raw) const
		{
			if (raw.empty()) { return {}; }
			return raw;
		}
		struct nested { virtual void not_mine(); };
		int value{ 3 };
		void plain();
		std::string_view kind_name() const override { return {}; }
	protected:
		virtual void invoke(task_context& ctx) noexcept = 0;
		virtual void unnamed(unsigned int, const char*) &&;
#define DEMO_MACRO(x) \\
	virtual void from_macro(x);
		virtual void unnamed_const(const T, long long) = 0;
	};
}
"""
    expected = ['~iface()', 'entt::id_type hash() const',
                'std::unique_ptr<slot> make(const state*, std::span<const participant>, int) const',
                'void bind(entt::registry&, std::string_view) const',
                'std::vector<std::byte> items(const ::res::tag&, const std::vector<std::byte>&) const',
                'void invoke(task_context&) noexcept', 'void unnamed(unsigned int, const char*) &&',
                'void unnamed_const(const T, long long)']
    got = pin_of(header, 'iface')
    expect(got == (['public base'], expected), f'the parser gives {got}')
    expect(revision_of(header, 'iface_abi_revision') == 1, 'the revision is not read')
    expect(virtuals_of(header, 'missing') is None, 'a missing class is found')

    # A `<` in parentheses compares; the braces of a member initializer are not the constructor's body.
    parsed = """struct parsed : virtual public a, protected  b<int, c::d>
	{
		parsed() : m_value{1}, m_base<int>{} {}
		virtual void after_ctor();
		virtual void ranged(int a = (1 < 2), float b = 0.5f) = 0;
		virtual void arrow(int a = p->x, std::function<void(int, int)> f = {}) = 0;
		int m_value;
	};
"""
    got = pin_of(parsed, 'parsed')
    expect(got == (['public virtual a', 'protected b<int, c::d>'],
                   ['void after_ctor()', 'void ranged(int, float)', 'void arrow(int, std::function<void(int, int)>)']),
           f'the parser gives {got}')

    key = 'using interface_abi_revisions = layout_values<demo::iface_abi_revision>;'
    layout = 'inline constexpr std::uint64_t layout_revision = 12;'
    editor_key = 'using editor_interface_abi_revisions = layout_values<>;'
    editor_layout = 'inline constexpr std::uint64_t editor_layout_revision = 1;'
    files = {'h.h': header, KEY_HEADER: key, LAYOUT_HEADER: layout, EDITOR_KEY_HEADER: editor_key,
             EDITOR_LAYOUT_HEADER: editor_layout}
    pins_in = {'interfaces': [{'class': 'iface', 'header': 'h.h', 'constant': 'iface_abi_revision'}]}
    pins, errors = update(pins_in, files.get)
    expect(not errors and pins['interfaces'][0]['virtuals'] == expected, f'a first --update gives {errors}')
    expect(pins['interfaces'][0]['bases'] == ['public base'], 'a first --update records no bases')
    expect(check(pins, files.get) == [], f'an unchanged tree gives {check(pins, files.get)}')

    renamed = dict(files, **{'h.h': header.replace('entt::registry& registry', 'entt::registry& reg')})
    expect(check(pins, renamed.get) == [], 'a renamed parameter is a change')

    reordered = dict(files, **{'h.h': header.replace('virtual ~iface() = default;', '').replace(
        'protected:', 'protected:\n\t\tvirtual ~iface() = default;')})
    errors = check(pins, reordered.get)
    expect(len(errors) == 1 and 'still 1' in errors[0] and 'to 2' in errors[0], f'a reorder gives {errors}')
    _, errors = update(pins, reordered.get)
    expect(len(errors) == 1 and 'raise iface_abi_revision' in errors[0], f'--update of a reorder gives {errors}')

    added = dict(files, **{'h.h': header.replace('void plain();', 'virtual void plain();')})
    expect(len(check(pins, added.get)) == 1, 'a new virtual is not a change')
    const_changed = dict(files, **{'h.h': header.replace('hash() const', 'hash()')})
    expect(len(check(pins, const_changed.get)) == 1, 'a lost const is not a change')
    rebased = dict(files, **{'h.h': header.replace('final : public base', 'final : public other')})
    errors = check(pins, rebased.get)
    expect(len(errors) == 1 and 'still 1' in errors[0] and 'public other' in errors[0], f'another base gives {errors}')
    _, errors = update(pins, rebased.get)
    expect(len(errors) == 1, f'--update of another base gives {errors}')

    bumped = dict(added, **{'h.h': added['h.h'].replace('iface_abi_revision = 1', 'iface_abi_revision = 2')})
    errors = check(pins, bumped.get)
    expect(len(errors) == 1 and '--update' in errors[0] and 'says 1' in errors[0], f'a raised revision gives {errors}')
    repinned, errors = update(pins, bumped.get)
    expect(not errors and check(repinned, bumped.get) == [], f'--update after a raise gives {errors}')

    unmixed = dict(files, **{KEY_HEADER: 'using interface_abi_revisions = layout_values<>;'})
    errors = check(pins, unmixed.get)
    expect(len(errors) == 1 and 'does not mix' in errors[0], f'a revision outside the key gives {errors}')

    # Against the base branch: the pin file edited by hand to match changed headers passes check(), not this.
    expect(check_against_base(pins, pins) == ([], []), 'an unchanged pin file differs from itself')
    hand_edited = dict(pins, interfaces=[dict(pins['interfaces'][0], virtuals=repinned['interfaces'][0]['virtuals'])])
    expect(check(hand_edited, added.get) == [], 'a hand-edited pin file does not match its headers')
    errors, _ = check_against_base(hand_edited, pins)
    expect(len(errors) == 1 and 'not higher' in errors[0] and 'void plain()' in errors[0], f'a hand edit gives {errors}')
    hand_rebased = {'interfaces': [dict(pins['interfaces'][0], bases=['public other'])]}
    errors, _ = check_against_base(hand_rebased, pins)
    expect(len(errors) == 1 and 'public other' in errors[0], f'a hand-edited base gives {errors}')
    expect(check_against_base(dict(repinned, layout_revision=13), pins) == ([], []),
           'a raised revision differs from the base branch')
    # A changed game pin needs layout_revision above the base's too (#969).
    errors, _ = check_against_base(repinned, pins)
    expect(len(errors) == 1 and 'layout_revision is 12 here' in errors[0], f'a change without a new layout gives {errors}')
    second = dict(pins['interfaces'][0], constant='other_abi_revision', virtuals=['void f()'])
    grown = {'interfaces': pins['interfaces'] + [second]}
    expect(check_against_base(grown, pins) == ([], []), 'a new pin differs from the base branch')
    expect(pins['layout_revision'] == 12, 'the layout revision is not recorded')

    # A pin of the base branch cannot vanish unless it is retired with a raised layout_revision.
    errors, _ = check_against_base(pins, grown)
    expect(len(errors) == 1 and 'other_abi_revision' in errors[0] and 'retired' in errors[0],
           f'a removed pin gives {errors}')
    retire = {'constant': 'other_abi_revision', 'reason': 'gone'}
    base_grown = dict(grown, layout_revision=12)
    retired_raised = dict(pins, layout_revision=13, retired=[retire])
    errors, _ = check_against_base(retired_raised, base_grown)
    expect(errors == [], f'a retired pin with a raised layout_revision gives {errors}')
    errors, _ = check_against_base(dict(pins, retired=[retire]), base_grown)
    expect(len(errors) == 1 and 'layout_revision' in errors[0], f'a retired pin, same layout_revision gives {errors}')
    errors, notes = check_against_base(retired_raised, {'interfaces': grown['interfaces']})
    expect(errors == [] and len(notes) == 1, f'a retirement against an older-format base gives {errors}, {notes}')
    renamed_constant = {'interfaces': [dict(pins['interfaces'][0], constant='iface2_abi_revision')]}
    errors, _ = check_against_base(renamed_constant, pins)
    expect(len(errors) == 1 and 'iface_abi_revision' in errors[0], f'a renamed constant gives {errors}')
    both = dict(pins, retired=[{'constant': 'iface_abi_revision', 'reason': 'x'}])
    expect(len(check(both, files.get)) == 1, 'a retired and active constant passes')

    # A changed list needs a revision above the base's, not only another one.
    def with_revision(source, revision):
        return {'layout_revision': 13, 'interfaces': [dict(source['interfaces'][0], revision=revision,
                                                           virtuals=repinned['interfaces'][0]['virtuals'])]}
    expect(check_against_base(with_revision(pins, 2), pins) == ([], []), 'a raised revision with a new list fails')
    errors, _ = check_against_base(with_revision(pins, 0), pins)
    expect(len(errors) == 1 and 'not higher' in errors[0], f'a lowered revision gives {errors}')
    errors, _ = check_against_base(with_revision(pins, 1), pins)
    expect(len(errors) == 1 and 'not higher' in errors[0], f'an equal revision gives {errors}')

    stale = dict(files, **{LAYOUT_HEADER: layout.replace('12', '13')})
    errors = check(pins, stale.get)
    expect(len(errors) == 1 and 'layout_revision is 13' in errors[0], f'another layout_revision gives {errors}')

    # The editor key (#969): an entry with "key": "editor" is mixed into the editor key header and its changes raise
    # editor_layout_revision; entries without "key" are the game's.
    editor_header = header.replace('iface', 'panel')
    editor_files = dict(files, **{'e.h': editor_header,
                                  EDITOR_KEY_HEADER: 'using editor_interface_abi_revisions = layout_values<panel_abi_revision>;'})
    both_in = {'interfaces': pins_in['interfaces'] + [
        {'class': 'panel', 'header': 'e.h', 'constant': 'panel_abi_revision', 'key': 'editor'}]}
    both, errors = update(both_in, editor_files.get)
    expect(not errors and both['editor_layout_revision'] == 1 and both['interfaces'][1]['key'] == 'editor'
           and 'key' not in both['interfaces'][0], f'--update of an editor pin gives {errors}')
    expect(check(both, editor_files.get) == [], f'an unchanged tree with an editor pin gives {check(both, editor_files.get)}')
    stale_editor = dict(editor_files, **{EDITOR_LAYOUT_HEADER: editor_layout.replace('1', '2')})
    errors = check(both, stale_editor.get)
    expect(len(errors) == 1 and 'editor_layout_revision is 2' in errors[0], f'another editor_layout_revision gives {errors}')

    # An editor revision constant the editor key header does not mix in, or one only the game key mixes in.
    unmixed_editor = dict(editor_files, **{EDITOR_KEY_HEADER: 'using editor_interface_abi_revisions = layout_values<>;',
                                           KEY_HEADER: key.replace('>', ', panel_abi_revision>')})
    errors = check(both, unmixed_editor.get)
    expect(len(errors) == 1 and 'does not mix panel_abi_revision' in errors[0]
           and 'editor_interface_abi_revisions' in errors[0], f'an editor revision outside the editor key gives {errors}')

    # An unknown key is an error, in check, --update and against the base branch.
    odd = {'interfaces': both['interfaces'][:1] + [dict(both['interfaces'][1], key='tools')],
           'layout_revision': 12, 'editor_layout_revision': 1}
    errors = check(odd, editor_files.get)
    expect(len(errors) == 1 and '"tools"' in errors[0], f'an unknown key gives {errors}')
    _, errors = update(odd, editor_files.get)
    expect(len(errors) == 1 and '"tools"' in errors[0], f'--update of an unknown key gives {errors}')
    errors, _ = check_against_base(odd, both)
    expect(len(errors) == 1 and '"tools"' in errors[0], f'an unknown key against the base gives {errors}')

    # An editor entry that changes needs editor_layout_revision above the base's, not layout_revision.
    changed_virtuals = both['interfaces'][1]['virtuals'] + ['void extra()']
    editor_changed = dict(both, interfaces=[both['interfaces'][0],
                                            dict(both['interfaces'][1], revision=2, virtuals=changed_virtuals)])
    errors, _ = check_against_base(dict(editor_changed, layout_revision=13), both)
    expect(len(errors) == 1 and 'editor_layout_revision is 1 here and 1 there' in errors[0],
           f'an editor change without a new editor_layout_revision gives {errors}')
    expect(check_against_base(dict(editor_changed, editor_layout_revision=2), both) == ([], []),
           'an editor change with a raised editor_layout_revision fails')
    # Only a game entry changes: no editor revision is needed.
    game_changed = dict(both, layout_revision=13, interfaces=[
        dict(both['interfaces'][0], revision=2, virtuals=repinned['interfaces'][0]['virtuals']), both['interfaces'][1]])
    expect(check_against_base(game_changed, both) == ([], []), 'a game change asks for an editor revision')
    # A base without editor_layout_revision reads as 0: the first editor pins pass, a change against it too.
    expect(check_against_base(both, pins) == ([], []), 'the first editor pins fail against a base without them')
    old_base = {key: value for key, value in both.items() if key != 'editor_layout_revision'}
    expect(check_against_base(editor_changed, old_base) == ([], []),
           'an editor change against a base without editor_layout_revision fails')
    # An editor pin is retired with a raised editor_layout_revision.
    without_panel = dict(both, interfaces=both['interfaces'][:1],
                         retired=[{'constant': 'panel_abi_revision', 'reason': 'gone'}])
    errors, _ = check_against_base(without_panel, both)
    expect(len(errors) == 1 and 'editor_layout_revision' in errors[0], f'an editor retirement, same revision gives {errors}')
    expect(check_against_base(dict(without_panel, editor_layout_revision=2), both) == ([], []),
           'an editor retirement with a raised editor_layout_revision fails')
    # A pin moved to another key retires it from the old key: the old key's revision must rise, the new key's need not.
    game_to_editor = dict(both, interfaces=[dict(both['interfaces'][0], key='editor'), both['interfaces'][1]])
    errors, _ = check_against_base(game_to_editor, both)
    expect(len(errors) == 1 and 'iface_abi_revision (moved to the editor key)' in errors[0]
           and 'layout_revision is 12 here' in errors[0], f'a game pin moved to the editor key gives {errors}')
    expect(check_against_base(dict(game_to_editor, layout_revision=13), both) == ([], []),
           'a game pin moved to the editor key with a raised layout_revision fails')
    editor_to_game = dict(both, interfaces=[both['interfaces'][0],
                                            {k: v for k, v in both['interfaces'][1].items() if k != 'key'}])
    errors, _ = check_against_base(editor_to_game, both)
    expect(len(errors) == 1 and 'panel_abi_revision (moved to the game key)' in errors[0]
           and 'editor_layout_revision is 1 here' in errors[0], f'an editor pin moved to the game key gives {errors}')
    expect(check_against_base(dict(editor_to_game, editor_layout_revision=2), both) == ([], []),
           'an editor pin moved to the game key with a raised editor_layout_revision fails')
    print(f'{TOOL}: self-test passed')


def main(argv):
    args = argv[1:]
    if args == ['--self-test']:
        self_test()
        return 0
    do_update = '--update' in args
    args = [a for a in args if a != '--update']
    options = {'--pins': DEFAULT_PINS, '--repo': '.', '--base-pins': None}
    while args:
        if len(args) < 2 or args[0] not in options:
            print(__doc__)
            return 2
        options[args[0]] = args[1]
        args = args[2:]
    if do_update and options['--base-pins'] is not None:
        print(f'{TOOL}: --base-pins checks, --update records: pass one of them')
        return 2
    repo = Path(options['--repo'])
    pins_path = repo / options['--pins']
    if not pins_path.is_file():
        print(f'{TOOL}: {pins_path} not found (run from the repository root or pass --repo)')
        return 2

    def read(path):
        file = repo / path
        return file.read_text(encoding='utf-8', errors='replace') if file.is_file() else None

    pins = load_pins(pins_path)
    if do_update:
        updated, errors = update(pins, read)
        for error in errors:
            print(error)
        if errors:
            print(f'{TOOL}: nothing recorded, {len(errors)} problem(s)')
            return 1
        pins_path.write_text(json.dumps(updated, indent=2) + '\n', encoding='utf-8', newline='\n')
        print(f'{TOOL}: recorded {len(updated["interfaces"])} interface(s) in {pins_path}')
        return 0
    errors = check(pins, read)
    if options['--base-pins'] is not None:
        base_errors, notes = check_against_base(pins, load_pins(options['--base-pins']))
        errors += base_errors
        for note in notes:
            print(note)
    for error in errors:
        print(error)
    if errors:
        print(f'{TOOL}: {len(errors)} problem(s)')
        return 1
    against = ' and the base branch\'s pins' if options['--base-pins'] is not None else ''
    print(f'{TOOL}: the base classes and virtual functions of {len(pins["interfaces"])} interface(s) match their '
          f'pinned ABI revisions{against}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
