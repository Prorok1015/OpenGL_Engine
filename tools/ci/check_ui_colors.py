#!/usr/bin/env python3
"""No literal colours in the editor's UI code (#1136).

    check_ui_colors.py              scan Editor/code/**/*.{cpp,h,hpp}; exit 1 with file:line for each literal colour
    check_ui_colors.py --self-test

The editor's panels draw states with the semantic colours of the active theme (gui::theme_semantic_colors:
panel_base::colors(), or editor_context::colors() passed in) and everything else with the ImGui style, so a theme
change reaches every window. A literal colour does not follow the theme; this script, run in the CI job Plan, refuses
a new one. A literal is:
  - IM_COL32(...) or ImColor(...) whose arguments are all numbers;
  - ImVec4(...) / ImVec4{...}, or an ImVec4 variable initialised the same way (ImVec4 x{...}, ImVec4 x(...)), whose
    three or four arguments are all numbers;
  - a brace list of exactly four floating-point numbers ({ 1.f, 0.3f, 0.3f, 1.f }), the shape of a colour passed as
    an ImVec4 without naming it (TextColored({...}), return {...}).
Comments and string literals are not read. EXCEPTIONS below allow a fixed number of literals of one kind in one file,
each with its reason; one more of that kind in that file fails as any other.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCANNED = ROOT / 'Editor' / 'code'
SUFFIXES = {'.cpp', '.h', '.hpp'}

# (path relative to the repository root, kind, how many, why)
EXCEPTIONS = [
	('Editor/code/editor_system/panels/edt_git_panel.cpp', 'IM_COL32', 8,
		'the lane colours of the history graph: told apart from each other, not states'),
	('Editor/code/editor_system/gizmo/edt_guizmo.hpp', 'IM_COL32', 7,
		'the orientation gizmo: the X/Y/Z axis colours every 3D tool shares'),
	('Editor/code/editor_system/panels/edt_schema_inspector.cpp', 'brace4', 1,
		'edit_color: the default of the colour value being edited (opaque black), data, not UI'),
]

NUMBER = r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?'
FLOAT = r'[-+]?(?:\d+\.\d*|\.\d+|\d+(?=[fF]))(?:[eE][-+]?\d+)?[fF]?'


def numbers(count_re, number=NUMBER):
	return r'\s*' + number + r'(?:\s*,\s*' + number + r')' + count_re + r'\s*'


PATTERNS = [
	('IM_COL32', re.compile(r'\bIM_COL32\s*\(' + numbers('{3}') + r'\)')),
	('ImColor', re.compile(r'\bImColor\s*[({]' + numbers('{2,3}') + r'[)}]')),
	('ImVec4', re.compile(r'\bImVec4\s*(?:[A-Za-z_]\w*\s*)?[({]' + numbers('{2,3}') + r'[)}]')),
	('brace4', re.compile(r'\{' + numbers('{3}', FLOAT) + r'\}')),
]


def strip_code(text):
	"""The text with comments and string/char literals blanked, line breaks kept."""
	out = []
	i = 0
	n = len(text)
	while i < n:
		c = text[i]
		if text.startswith('//', i):
			j = text.find('\n', i)
			j = n if j < 0 else j
			out.append(' ' * (j - i))
			i = j
		elif text.startswith('/*', i):
			j = text.find('*/', i + 2)
			j = n if j < 0 else j + 2
			out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
			i = j
		elif text.startswith('R"', i):
			m = re.match(r'R"([^(\s]{0,16})\(', text[i:])
			if not m:
				out.append(c)
				i += 1
				continue
			end = text.find(')' + m.group(1) + '"', i)
			j = n if end < 0 else end + len(m.group(1)) + 2
			out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
			i = j
		elif c in '"\'':
			j = i + 1
			while j < n and text[j] != c and text[j] != '\n':
				j += 2 if text[j] == '\\' else 1
			j = min(j + 1, n)
			out.append(c + ' ' * max(0, j - i - 2) + (c if j - i >= 2 else ''))
			i = j
		else:
			out.append(c)
			i += 1
	return ''.join(out)


def find_literals(text):
	"""[(line, kind, snippet)] of the literal colours in C++ source `text`."""
	code = strip_code(text)
	found = []
	spans = []
	for kind, pattern in PATTERNS:
		for m in pattern.finditer(code):
			# The braces of ImVec4{...} are that literal, not a second one.
			if any(start <= m.start() and m.end() <= end for start, end in spans):
				continue
			spans.append(m.span())
			line = code.count('\n', 0, m.start()) + 1
			found.append((line, kind, ' '.join(m.group(0).split())))
	found.sort()
	return found


def scan(root, scanned):
	problems = []
	allowed = {(path, kind): (count, why) for path, kind, count, why in EXCEPTIONS}
	used = {}
	for file in sorted(scanned.rglob('*')):
		if file.suffix not in SUFFIXES or not file.is_file():
			continue
		relative = file.relative_to(root).as_posix()
		text = file.read_text(encoding='utf-8', errors='replace')
		for line, kind, snippet in find_literals(text):
			key = (relative, kind)
			if key in allowed:
				used[key] = used.get(key, 0) + 1
				if used[key] <= allowed[key][0]:
					continue
			problems.append(f'{relative}:{line}: literal colour {snippet}')
	for key, (count, _) in allowed.items():
		if used.get(key, 0) < count:
			problems.append(f'{key[0]}: the exception for {count} {key[1]} literal(s) now covers {used.get(key, 0)}: '
				'lower its count in tools/ci/check_ui_colors.py')
	return problems


def self_test():
	cases = [
		('ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "x");', ['ImVec4']),
		('ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.f, 0.3f, 0.3f, 1.f });', ['ImVec4']),
		('const ImVec4 k_error{ 1.0f, 0.45f, 0.40f, 1.0f };', ['ImVec4']),
		('const ImVec4 error_color(1.0f, 0.4f, 0.4f, 1.0f);', ['ImVec4']),
		('ImU32 c = IM_COL32(80, 160, 255, 200);', ['IM_COL32']),
		('ImU32 c = ImColor(255, 0, 0);', ['ImColor']),
		('ImGui::TextColored({ 1.f, 0.8f, 0.f, 1.f }, "x");', ['brace4']),
		('case x: return { 0.45f, 0.75f, 1.f, 1.f };', ['brace4']),
		('return ImVec4(c.x * f, c.y * f, c.z * f, c.w);', []),
		('ImGui::TextColored(colors().error, "x");', []),
		('constexpr int quads[1][4] = { { 4, 5, 6, 7 } };', []),
		('ImGui::Text("ImVec4(1.f, 0.f, 0.f, 1.f)");', []),
		('// ImVec4(1.f, 0.f, 0.f, 1.f)', []),
		('/* IM_COL32(1, 2, 3, 4) */ int x;', []),
		('ImGui::SetNextWindowSize(ImVec2(520.f, 460.f));', []),
		('ImU32 c = ImGui::GetColorU32(ImGuiCol_CheckMark, 0.8f);', []),
	]
	failed = 0
	for source, kinds in cases:
		got = sorted(kind for _, kind, _ in find_literals(source))
		if got != sorted(kinds):
			print(f'self-test: {source!r}: expected {sorted(kinds)}, got {got}')
			failed += 1
	if failed:
		return 1
	print(f'self-test: {len(cases)} cases passed')
	return 0


def main(argv):
	if argv[1:] == ['--self-test']:
		return self_test()
	if len(argv) > 1:
		print(__doc__)
		return 2
	problems = scan(ROOT, SCANNED)
	for problem in problems:
		print(problem)
	if problems:
		print(f'{len(problems)} literal colour(s) in Editor/code: take a semantic colour of the theme '
			'(panel_base::colors(), editor_context::colors()) or a colour of the ImGui style instead, '
			'or add an exception with its reason')
		return 1
	print('no literal colours in Editor/code')
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv))
