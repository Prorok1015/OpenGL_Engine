#!/usr/bin/env python3
"""No pixel-constant sizes in the editor's UI code (#1136).

    check_ui_sizes.py              scan Editor/code/**/*.{cpp,h,hpp}; exit 1 with file:line for each literal size
    check_ui_sizes.py --self-test

A size written in pixels does not follow the font size (and so the DPI): a panel wraps it in edt::em(...) (scaled by
the font size, core/edt_ui_metrics.h) or edt::dpi_px(...) (by the DPI only, for the few sizes of the default layout).
This script, run in the CI job Plan, refuses a numeric literal with an absolute value above 2 outside those calls in
the size argument of:
  - SetNextItemWidth, PushItemWidth (the width), SetNextWindowSize (the size), SetNextWindowSizeConstraints (both);
  - SetColumnWidth (the width), TableSetupColumn (the width of a column that is WidthFixed);
  - BeginChild, Button, InvisibleButton, ProgressBar (the size);
  - SameLine (the offset and the spacing).
-1, 0 and the like (fill the space, auto size) are not read. Comments and string literals are not read. EXCEPTIONS
below allow a fixed number of literals in one file, each with its reason; one more in that file fails as any other.
"""
import re
import string
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCANNED = ROOT / 'Editor' / 'code'
SUFFIXES = {'.cpp', '.h', '.hpp'}

# (path relative to the repository root, how many, why)
EXCEPTIONS = [
]

# function -> indices of its arguments that are sizes
SIZE_ARGS = {
	'SetNextItemWidth': (0,),
	'PushItemWidth': (0,),
	'SetNextWindowSize': (0,),
	'SetNextWindowSizeConstraints': (0, 1),
	'SetColumnWidth': (1,),
	'BeginChild': (1,),
	'Button': (1,),
	'InvisibleButton': (1,),
	'ProgressBar': (1,),
	'SameLine': (0, 1),
}
CALL = re.compile(r'\b(?:ImGui::)?(' + '|'.join(SIZE_ARGS) + r'|TableSetupColumn)\s*\(')
SCALED = re.compile(r'\b(?:edt::)?(?:em|dpi_px)\s*\(')
NUMBER = re.compile(r'(?<![\w.])(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?(?![\w.])')
# An argument that is a multiple of one of these already follows the font size.
FONT_RELATIVE = re.compile(r'\b(?:GetFontSize|GetFrameHeight|GetTextLineHeight\w*|CalcTextSize)\s*\(')
LIMIT = 2.0


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


def close_of(code, open_at):
	"""Index of the bracket closing the one at `open_at`, or -1."""
	depth = 0
	for i in range(open_at, len(code)):
		if code[i] in '([{':
			depth += 1
		elif code[i] in ')]}':
			depth -= 1
			if depth == 0:
				return i
	return -1


def split_args(text):
	args = []
	depth = 0
	start = 0
	for i, c in enumerate(text):
		if c in '([{':
			depth += 1
		elif c in ')]}':
			depth -= 1
		elif c == ',' and depth == 0:
			args.append(text[start:i])
			start = i + 1
	args.append(text[start:])
	return args


def blank_scaled(arg):
	"""`arg` with the arguments of em(...) / dpi_px(...) blanked."""
	while True:
		m = SCALED.search(arg)
		if not m:
			return arg
		end = close_of(arg, m.end() - 1)
		end = len(arg) - 1 if end < 0 else end
		arg = arg[:m.start()] + ' ' * (end + 1 - m.start()) + arg[end + 1:]


def literal_of(arg):
	"""The first numeric literal above the limit in `arg`, or None."""
	if FONT_RELATIVE.search(arg):
		return None
	code = blank_scaled(arg)
	for m in NUMBER.finditer(code):
		before = code[:m.start()].rstrip()
		# A negative size (-40: to the end of the space) is a fill, not pixels; `a - 40` is a pixel size.
		operand_end = tuple(')]}') + tuple(string.ascii_letters + string.digits + '_.')
		if before.endswith('-') and not before[:-1].rstrip().endswith(operand_end):
			continue
		if float(m.group(0).rstrip('fF')) > LIMIT:
			return m.group(0)
	return None


def find_literals(text):
	"""[(line, function, literal)] of the pixel-constant sizes in C++ source `text`."""
	code = strip_code(text)
	found = []
	for m in CALL.finditer(code):
		end = close_of(code, m.end() - 1)
		if end < 0:
			continue
		name = m.group(1)
		args = split_args(code[m.end():end])
		if name == 'TableSetupColumn':
			# Only a fixed column has a width in pixels; a stretch column has a weight.
			if len(args) < 3 or 'WidthFixed' not in args[1]:
				continue
			indices = (2,)
		else:
			indices = SIZE_ARGS[name]
		for index in indices:
			if index < len(args):
				literal = literal_of(args[index])
				if literal is not None:
					found.append((code.count('\n', 0, m.start()) + 1, name, literal))
	found.sort()
	return found


def scan(root, scanned):
	problems = []
	allowed = {path: (count, why) for path, count, why in EXCEPTIONS}
	used = {}
	for file in sorted(scanned.rglob('*')):
		if file.suffix not in SUFFIXES or not file.is_file():
			continue
		relative = file.relative_to(root).as_posix()
		text = file.read_text(encoding='utf-8', errors='replace')
		for line, name, literal in find_literals(text):
			if relative in allowed:
				used[relative] = used.get(relative, 0) + 1
				if used[relative] <= allowed[relative][0]:
					continue
			problems.append(f'{relative}:{line}: {name}: pixel size {literal} outside em(...) / dpi_px(...)')
	for path, (count, _) in allowed.items():
		if used.get(path, 0) < count:
			problems.append(f'{path}: the exception for {count} literal size(s) now covers {used.get(path, 0)}: '
				'lower its count in tools/ci/check_ui_sizes.py')
	return problems


def self_test():
	cases = [
		('ImGui::SetNextItemWidth(120.f);', ['SetNextItemWidth']),
		('ImGui::PushItemWidth(-1);', []),
		('ImGui::SetNextItemWidth(em(120.f));', []),
		('ImGui::SetNextItemWidth(avail - 80.f);', ['SetNextItemWidth']),
		('ImGui::SetNextItemWidth(edt::dpi_px(160));', []),
		('ImGui::SetNextWindowSize(ImVec2(520.f, 460.f));', ['SetNextWindowSize']),
		('ImGui::SetNextWindowSize(em(ImVec2(520.f, 460.f)), ImGuiCond_FirstUseEver);', []),
		('ImGui::SetNextWindowSize(ImVec2(em(520.f), em(460.f)));', []),
		('ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(FLT_MAX, FLT_MAX));',
		 ['SetNextWindowSizeConstraints']),
		('ImGui::SetColumnWidth(0, 160);', ['SetColumnWidth']),
		('ImGui::SetColumnWidth(0, dpi_px(160));', []),
		('ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 70.f);', ['TableSetupColumn']),
		('ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, em(70.f));', []),
		('ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 3.f);', []),
		('ImGui::TableSetupColumn("a");', []),
		('ImGui::BeginChild("c", ImVec2(0, 120), true);', ['BeginChild']),
		('ImGui::BeginChild("c", ImVec2(0, -40), true);', []),
		('ImGui::BeginChild("c", ImVec2(0, em(120)), true);', []),
		('ImGui::Button("OK", ImVec2(80, 0));', ['Button']),
		('ImGui::Button("OK", ImVec2(-1, 0));', []),
		('ImGui::Button("OK");', []),
		('ImGui::InvisibleButton("x", ImVec2(24, 24));', ['InvisibleButton']),
		('ImGui::ProgressBar(0.5f, ImVec2(200, 0));', ['ProgressBar']),
		('ImGui::ProgressBar(0.5f, ImVec2(-1, 0));', []),
		('ImGui::SameLine(0, 8);', ['SameLine']),
		('ImGui::SameLine(0, 2);', []),
		('ImGui::SameLine(0.f, em(8.f));', []),
		('ImGui::SameLine();', []),
		('ImGui::Button(label, ImVec2(w3, 0));', []),
		('ImGui::Button("Size 100 px", ImVec2(0, 0));', []),
		('ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);', []),
		('ImGui::Button("x", ImVec2(ImGui::CalcTextSize("abc").x + 40.f, 0));', []),
		('// ImGui::SetNextItemWidth(120.f);', []),
		('ImGui::SetNextItemWidth(\n\t120.f);', ['SetNextItemWidth']),
	]
	failed = 0
	for source, expected in cases:
		got = [name for _, name, _ in find_literals(source)]
		if got != expected:
			print(f'self-test: {source!r}: expected {expected}, got {got}')
			failed += 1
	print(f'self-test: {len(cases) - failed}/{len(cases)} ok')
	return failed == 0


def main():
	if '--self-test' in sys.argv[1:]:
		return 0 if self_test() else 1
	problems = scan(ROOT, SCANNED)
	for problem in problems:
		print(problem)
	if problems:
		print(f'{len(problems)} problem(s): size the UI with edt::em(...) (follows the font size) or edt::dpi_px(...) '
			'(the DPI only), see Editor/code/editor_system/core/edt_ui_metrics.h, #1136')
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main())
