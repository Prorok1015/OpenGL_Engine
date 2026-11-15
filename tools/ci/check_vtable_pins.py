#!/usr/bin/env python3
"""Fails when the virtual functions of an engine interface a module calls or fills change and its ABI revision does not (#922).

    check_vtable_pins.py [--pins <file>] [--repo <root>]            check (default: tools/ci/vtable_pins.json, .)
    check_vtable_pins.py --update [--pins <file>] [--repo <root>]   record the current lists and revisions
    check_vtable_pins.py --self-test

A game module built against an engine fills or calls the vtable of these interfaces inline, by slot. The layout key
(gs::module_layout_key, snk/game/gs_module_layout_key.h) sees no vtable: it mixes in the <interface>_abi_revision
constant declared next to each interface instead. The pin file holds, per interface, its header, the constant, the
revision and the ordered list of the virtual functions the class declares. The check fails when:
  - the list in the header differs from the pinned one while the constant still has the pinned revision: raise the
    constant by one (gs_module_layout.h raises layout_revision with it, see the rule above SNK_MODULE_API_VERSION) and
    record the new list with --update;
  - the constant differs from the pinned revision: record it with --update;
  - the header, the class or the constant is gone, or the key header does not name the constant.
--update refuses the first case too: it records a changed list only with a raised revision.

What is pinned: each declaration in the class body (not in a nested class) that contains the word `virtual`, in order,
as `<return type> <name>(<parameter types>) <qualifiers>`: parameter names, default arguments, `virtual`, `override`,
`final`, `= 0`, `= default`, an inline body and [[attributes]] are dropped, spaces normalized. So a reorder, a new or a
removed virtual, another parameter or return type, const, &/&& or noexcept count as a change; a renamed parameter, a
comment or a new body do not.

It is a text parser, not a compiler. It does not see:
  - a virtual function brought in by a macro, or a class declared by one;
  - an override written without `virtual` (it adds no slot to a single-inheritance vtable; a covariant return can);
  - a change of a typedef or alias a signature names (the spelling is compared, not the type);
  - preprocessor branches: every #if branch is read as if it were taken.
A parameter that is a function pointer or an array keeps its name in the pinned text.
"""
import json
import re
import sys
from pathlib import Path

DEFAULT_PINS = 'tools/ci/vtable_pins.json'
KEY_HEADER = 'core/engine/game_system/include/snk/game/gs_module_layout_key.h'
TOOL = 'check_vtable_pins'

BUILTIN_WORDS = {'bool', 'char', 'char8_t', 'char16_t', 'char32_t', 'wchar_t', 'short', 'int', 'long', 'signed',
                 'unsigned', 'float', 'double', 'void', 'auto', 'const', 'volatile'}
IDENT = re.compile(r'[A-Za-z_]\w*')


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


def class_body(text, name):
    """The text between the braces of the definition of class/struct `name`, or None."""
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
        return text[brace + 1:find_matching(text, brace, '{', '}')]
    return None


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
            if '(' in head:
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


def split_top_level(text, separator=','):
    parts = []
    depth = 0
    current = []
    for c in text:
        if c in '(<[{':
            depth += 1
        elif c in ')>]}':
            depth -= 1
        if c == separator and depth == 0:
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


def virtuals_of(header_text, class_name):
    """The ordered signatures of the virtual functions `class_name` declares, or None when it is not defined."""
    body = class_body(skip_preprocessor(strip_comments(header_text)), class_name)
    if body is None:
        return None
    return [signature(d) for d in member_declarations(body) if re.search(r'\bvirtual\b', d) and '(' in d]


def revision_of(header_text, constant):
    match = re.search(r'\b' + re.escape(constant) + r'\s*=\s*(\d+)\s*;', strip_comments(header_text))
    return int(match.group(1)) if match else None


def current_state(entry, read):
    """(virtuals, revision, problems) of one pinned interface; `read` maps a repository path to its text or None."""
    text = read(entry['header'])
    if text is None:
        return None, None, [f"{entry['header']}: not found (pinned interface {entry['class']})"]
    problems = []
    virtuals = virtuals_of(text, entry['class'])
    if virtuals is None:
        problems.append(f"{entry['header']}: no definition of class {entry['class']}")
    revision = revision_of(text, entry['constant'])
    if revision is None:
        problems.append(f"{entry['header']}: no `inline constexpr std::uint32_t {entry['constant']} = <n>;`")
    return virtuals, revision, problems


def check(pins, read):
    errors = []
    key_text = read(KEY_HEADER)
    for entry in pins['interfaces']:
        virtuals, revision, problems = current_state(entry, read)
        errors += problems
        if key_text is not None and not re.search(r'\b' + re.escape(entry['constant']) + r'\b', strip_comments(key_text)):
            errors.append(f"{KEY_HEADER}: does not mix {entry['constant']} into the key (interface_abi_revisions)")
        if problems:
            continue
        pinned = entry.get('virtuals')
        pinned_revision = entry.get('revision')
        where = f"{entry['header']}: {entry['class']}"
        if pinned is None:
            errors.append(f'{where}: not pinned yet: run python3 tools/ci/check_vtable_pins.py --update')
        elif virtuals != pinned and revision == pinned_revision:
            errors.append(f'{where}: its virtual functions changed but {entry["constant"]} is still {revision}. A module '
                          f'built before calls them by slot: raise {entry["constant"]} to {revision + 1} and layout_revision '
                          f'in gs_module_layout.h, then run python3 tools/ci/check_vtable_pins.py --update.\n'
                          + describe_change(pinned, virtuals))
        elif revision != pinned_revision:
            errors.append(f'{where}: {entry["constant"]} is {revision}, the pin file says {pinned_revision}: run '
                          f'python3 tools/ci/check_vtable_pins.py --update and commit {DEFAULT_PINS}')
    return errors


def describe_change(before, after):
    lines = ['    pinned:'] + [f'      {s}' for s in before] + ['    now:'] + [f'      {s}' for s in after]
    return '\n'.join(lines)


def update(pins, read):
    """The pins with the current lists and revisions, or the errors that forbid recording them."""
    errors = []
    updated = []
    for entry in pins['interfaces']:
        virtuals, revision, problems = current_state(entry, read)
        errors += problems
        if problems:
            continue
        pinned = entry.get('virtuals')
        if pinned is not None and virtuals != pinned and revision <= entry.get('revision', 0):
            errors.append(f"{entry['header']}: {entry['class']}: its virtual functions changed: raise {entry['constant']} "
                          f"above {entry.get('revision')} before recording them\n" + describe_change(pinned, virtuals))
            continue
        updated.append({'class': entry['class'], 'header': entry['header'], 'constant': entry['constant'],
                        'revision': revision, 'virtuals': virtuals})
    return ({'interfaces': updated} if not errors else None), errors


def expect(condition, what):
    # Not assert: python -O would skip every check.
    if not condition:
        raise SystemExit(f'{TOOL}: self-test failed: {what}')


def self_test():
    header = '''#pragma once
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
'''
    expected = ['~iface()', 'entt::id_type hash() const',
                'std::unique_ptr<slot> make(const state*, std::span<const participant>, int) const',
                'void bind(entt::registry&, std::string_view) const',
                'std::vector<std::byte> items(const ::res::tag&, const std::vector<std::byte>&) const',
                'void invoke(task_context&) noexcept', 'void unnamed(unsigned int, const char*) &&',
                'void unnamed_const(const T, long long)']
    got = virtuals_of(header, 'iface')
    expect(got == expected, f'the parser gives {got}')
    expect(revision_of(header, 'iface_abi_revision') == 1, 'the revision is not read')
    expect(virtuals_of(header, 'missing') is None, 'a missing class is found')

    key = 'using interface_abi_revisions = layout_values<demo::iface_abi_revision>;'
    files = {'h.h': header, KEY_HEADER: key}
    pins_in = {'interfaces': [{'class': 'iface', 'header': 'h.h', 'constant': 'iface_abi_revision'}]}
    pins, errors = update(pins_in, files.get)
    expect(not errors and pins['interfaces'][0]['virtuals'] == expected, f'a first --update gives {errors}')
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

    bumped = dict(added, **{'h.h': added['h.h'].replace('iface_abi_revision = 1', 'iface_abi_revision = 2')})
    errors = check(pins, bumped.get)
    expect(len(errors) == 1 and '--update' in errors[0] and 'says 1' in errors[0], f'a raised revision gives {errors}')
    repinned, errors = update(pins, bumped.get)
    expect(not errors and check(repinned, bumped.get) == [], f'--update after a raise gives {errors}')

    unmixed = dict(files, **{KEY_HEADER: 'using interface_abi_revisions = layout_values<>;'})
    errors = check(pins, unmixed.get)
    expect(len(errors) == 1 and 'does not mix' in errors[0], f'a revision outside the key gives {errors}')
    print(f'{TOOL}: self-test passed')


def main(argv):
    args = argv[1:]
    if args == ['--self-test']:
        self_test()
        return 0
    do_update = '--update' in args
    args = [a for a in args if a != '--update']
    options = {'--pins': DEFAULT_PINS, '--repo': '.'}
    while args:
        if len(args) < 2 or args[0] not in options:
            print(__doc__)
            return 2
        options[args[0]] = args[1]
        args = args[2:]
    repo = Path(options['--repo'])
    pins_path = repo / options['--pins']
    if not pins_path.is_file():
        print(f'{TOOL}: {pins_path} not found (run from the repository root or pass --repo)')
        return 2

    def read(path):
        file = repo / path
        return file.read_text(encoding='utf-8', errors='replace') if file.is_file() else None

    pins = json.loads(pins_path.read_text(encoding='utf-8'))
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
    for error in errors:
        print(error)
    if errors:
        print(f'{TOOL}: {len(errors)} problem(s)')
        return 1
    print(f'{TOOL}: the virtual functions of {len(pins["interfaces"])} interface(s) match their pinned ABI revisions')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
