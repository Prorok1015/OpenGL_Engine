#!/usr/bin/env python3
"""Fails when an installed snake_sdk exposes base-only API (<component>/include_base, MODULE_LAYER.md, #405).

  check_sdk_base_only.py <sdk prefix> [--repo <root>]
  check_sdk_base_only.py --self-test [--repo <root>]

The base-only names come from the include_base trees of the repository: their header paths, the functions they
declare with SNK_API, the classes and structs they define and their macros. An installed header (bundled third-party
trees aside) fails when it is one of those paths, includes one, defines one of the types or macros, declares one of
the functions with SNK_API, or uses one of the macros. --self-test plants each kind of leak in a scratch prefix and
fails unless every one is caught and a clean header passes.
"""
import os
import re
import sys
import tempfile
from pathlib import Path

THIRD_PARTY = re.compile(r'^snk/(boost|lib3dparty|_generated)/')
INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
SNK_API_FUNC = re.compile(r'\bSNK_API\b[^;{}()]*?\b([A-Za-z_]\w*)\s*\(')
TYPE_DEF = re.compile(r'\b(?:struct|class)\s+(?:SNK_API\s+)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{]*)?\{')
MACRO_DEF = re.compile(r'#\s*define\s+([A-Za-z_]\w*)')
SKIPPED_DIRS = {'.git', 'lib3dparty', 'node_modules'}


def strip_comments(text):
	text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
	return re.sub(r'//[^\n]*', '', text)


def base_only_api(repo):
	files, funcs, types, macros = set(), set(), set(), set()
	for current, dirs, names in os.walk(repo):
		dirs[:] = [d for d in dirs if d not in SKIPPED_DIRS and not d.startswith('build')]
		if 'include_base' not in Path(current).parts:
			continue
		base = Path(current)
		while base.name != 'include_base':
			base = base.parent
		for name in names:
			if not name.endswith(('.h', '.hpp', '.hh', '.hxx', '.inl', '.ipp')):
				continue
			path = Path(current) / name
			files.add(path.relative_to(base).as_posix())
			text = strip_comments(path.read_text(encoding='utf-8', errors='replace'))
			funcs.update(SNK_API_FUNC.findall(text))
			types.update(TYPE_DEF.findall(text))
			macros.update(MACRO_DEF.findall(text))
	return files, funcs, types, macros


def leaks(prefix, api):
	files, funcs, types, macros = api
	root = Path(prefix) / 'include'
	found = []
	for path in sorted(root.rglob('*')):
		if not path.is_file():
			continue
		rel = path.relative_to(root).as_posix()
		if THIRD_PARTY.match(rel):
			continue
		if rel in files:
			found.append(f'{rel}  (base-only header)')
			continue
		text = strip_comments(path.read_text(encoding='utf-8', errors='replace'))
		reasons = []
		reasons += [f'includes {inc}' for inc in INCLUDE.findall(text) if inc in files]
		reasons += [f'defines {name}' for name in TYPE_DEF.findall(text) if name in types]
		reasons += [f'declares {name}' for name in SNK_API_FUNC.findall(text) if name in funcs]
		reasons += [f'defines {name}' for name in MACRO_DEF.findall(text) if name in macros]
		used = {m for m in macros if re.search(rf'\b{re.escape(m)}\b', text)}
		reasons += [f'uses {name}' for name in sorted(used) if f'defines {name}' not in reasons]
		if reasons:
			found.append(f'{rel}  ({", ".join(reasons)})')
	return found


def self_test(api):
	files, funcs, types, macros = api
	if not (files and funcs and types and macros):
		print('::error::check_sdk_base_only: found no base-only headers, functions, types or macros to check against')
		return 1
	func, type_name, macro, file = sorted(funcs)[0], sorted(types)[0], sorted(macros)[0], sorted(files)[0]
	planted = {
		'function on its own line': f'#pragma once\nSNK_API\nvoid {func}(int value);\n',
		'type': f'#pragma once\nstruct {type_name} {{ int x; }};\n',
		'macro use': f'#pragma once\ninline int f() {{ return {macro}; }}\n',
		'include': f'#pragma once\n#include <{file}>\n',
	}
	clean = '#pragma once\n// mentions ' + func + ' in a comment only\nnamespace x { void ' + func + 'er(); }\n'
	failed = 0
	with tempfile.TemporaryDirectory() as tmp:
		for label, text in planted.items():
			prefix = Path(tmp) / label.replace(' ', '_')
			(prefix / 'include/snk/x').mkdir(parents=True)
			(prefix / 'include/snk/x/leak.h').write_text(text)
			if not leaks(prefix, api):
				print(f'::error::check_sdk_base_only self-test: a planted leak ({label}) was not caught')
				failed = 1
		prefix = Path(tmp) / 'file'
		(prefix / 'include' / Path(file).parent).mkdir(parents=True)
		(prefix / 'include' / file).write_text('#pragma once\n')
		if not leaks(prefix, api):
			print('::error::check_sdk_base_only self-test: a base-only header installed as is was not caught')
			failed = 1
		prefix = Path(tmp) / 'clean'
		(prefix / 'include/snk/x').mkdir(parents=True)
		(prefix / 'include/snk/x/clean.h').write_text(clean)
		if found := leaks(prefix, api):
			print(f'::error::check_sdk_base_only self-test: a clean header was reported: {found}')
			failed = 1
	if not failed:
		print(f'base-only self-test: ok ({len(files)} headers, {len(funcs)} functions, {len(types)} types, {len(macros)} macros)')
	return failed


def main(argv):
	repo = Path(__file__).resolve().parents[2]
	if '--repo' in argv:
		repo = Path(argv[argv.index('--repo') + 1])
	api = base_only_api(repo)
	if '--self-test' in argv:
		return self_test(api)
	prefix = argv[1] if len(argv) > 1 and not argv[1].startswith('--') else None
	if prefix is None:
		print('usage: check_sdk_base_only.py <sdk prefix> [--repo <root>] | --self-test')
		return 2
	found = leaks(prefix, api)
	for entry in found:
		print(f'  base-only: {entry}')
	if found:
		print(f'::error::{len(found)} installed header(s) expose base-only API (include_base/, MODULE_LAYER.md)')
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv))
