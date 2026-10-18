#!/usr/bin/env python3
"""Include-rewrite tool for the physical public/ header split (issue #109).

Design: docs/roadmap/design/HEADER_SPLIT.md. Python 3, stdlib only.

    split.py plan  <component|PR-N>...   what would move / change (read-only)
    split.py apply <component|PR-N>...   git mv public headers + rewrite includes
    split.py check                       repo-wide invariants
    split.py report                      markdown dry-run report (all PR groups)

All file IO on sources is binary: EOLs (CRLF/LF) and every byte outside an
include path are preserved.
"""
import argparse
import collections
import json
import os
import posixpath
import re
import subprocess
import sys

SCAN_DIRS = ("core", "Editor", "tests", "tools")
HEADER_EXT = (".h", ".hpp", ".hxx", ".inl", ".ipp")
SOURCE_EXT = HEADER_EXT + (".cpp", ".cc", ".cxx", ".c", ".mm")
SKIP_PATHS = {"tools/header_split"}
EXTERNAL = "<external>"  # files outside every component root (Editor, tests, tools)

# Group 1 keeps the directive prefix, 2 = "quoted" path, 3 = <angle> path.
INCLUDE_RE = re.compile(rb'^([ \t]*#[ \t]*include[ \t]*)(?:"([^"\r\n]+)"|<([^>\r\n]+)>)', re.M)
LONE_LF_RE = re.compile(rb'(?<!\r)\n')


class ToolError(Exception):
	pass


class Header:
	def __init__(self, path):
		self.path = path          # current repo-relative path (posix)
		self.comp = None          # component name or None
		self.public = None        # True / False / None (unclassified)
		self.sub = None           # path relative to component root
		self.canonical = None     # "snk/<name>/<sub>" for public headers
		self.moved = False        # already lives under <root>/include/<dest>/

	@property
	def internal(self):
		return self.public is False


class Edge:
	"""One #include line: where it is, what it says, what it resolves to."""
	def __init__(self, path, line, start, end, prefix, inc, quoted, cands):
		self.path, self.line, self.start, self.end = path, line, start, end
		self.prefix, self.inc, self.quoted, self.cands = prefix, inc, quoted, cands

	@property
	def target(self):
		return self.cands[0] if len(self.cands) == 1 else None


class Repo:
	def __init__(self, root, config_path):
		self.root = os.path.abspath(root)
		with open(config_path, encoding="utf-8") as f:
			cfg = json.load(f)
		self.comps = {c["name"]: c for c in cfg["components"]}
		self.groups = cfg.get("pr_groups", {})
		self.headers = {}
		self.by_base = collections.defaultdict(list)
		self.sources = []
		self._scan()
		self._overlay_config()
		self._edges = None

	# ---- indexing -------------------------------------------------------
	def _scan(self):
		for top in SCAN_DIRS:
			for d, dirs, files in os.walk(os.path.join(self.root, top)):
				rel_d = os.path.relpath(d, self.root).replace(os.sep, "/")
				dirs[:] = sorted(x for x in dirs if not self._skip_dir(rel_d + "/" + x, x))
				for name in sorted(files):
					if name.endswith(SOURCE_EXT):
						self.sources.append(rel_d + "/" + name)
		for p in self.sources:
			if p.endswith(HEADER_EXT):
				self._add_header(Header(p))

	@staticmethod
	def _skip_dir(rel, name):
		return (rel in SKIP_PATHS or name in ("lib3dparty", ".git", "node_modules")
			or name.startswith(("build", "cmake-build")))

	def _add_header(self, h):
		self.headers[h.path] = h
		self.by_base[posixpath.basename(h.path)].append(h)

	def _overlay_config(self):
		for name, c in self.comps.items():
			for kind, is_public in (("public", True), ("internal", False)):
				for sub in c.get(kind, []):
					self._claim(c, sub, is_public)

	def _claim(self, c, sub, is_public):
		old = posixpath.join(c["root"], sub)
		new = posixpath.join(c["root"], "include", c["dest"], sub) if c["dest"] and is_public else None
		in_old, in_new = old in self.headers, bool(new) and new in self.headers
		if in_old and in_new:
			raise ToolError(f"{c['name']}: {sub} exists both at {old} and {new}")
		if not (in_old or in_new):
			raise ToolError(f"{c['name']}: listed header not found: {old}")
		h = self.headers[new if in_new else old]
		if h.comp:
			raise ToolError(f"{sub}: claimed by both {h.comp} and {c['name']}")
		h.comp, h.public, h.sub, h.moved = c["name"], is_public, sub, in_new
		if is_public and c["dest"]:
			h.canonical = f"{c['dest']}/{sub}"

	def new_path(self, h):
		c = self.comps[h.comp]
		return posixpath.join(c["root"], "include", c["dest"], h.sub)

	def owner(self, path):
		"""Component whose root is the longest prefix of path, else EXTERNAL."""
		best = None
		for name, c in self.comps.items():
			if path.startswith(c["root"] + "/") and (best is None or len(c["root"]) > len(self.comps[best]["root"])):
				best = name
		return best or EXTERNAL

	# ---- include resolution --------------------------------------------
	def resolve(self, includer, inc, quoted):
		inc = inc.replace("\\", "/")
		if inc.startswith("./"):
			inc = inc[2:]
		if quoted:
			local = posixpath.normpath(posixpath.join(posixpath.dirname(includer), inc))
			if local in self.headers:
				return [self.headers[local]]
		if ".." in inc.split("/"):
			return []
		suffix = "/" + inc
		return [h for h in self.by_base.get(posixpath.basename(inc), ())
			if h.path == inc or h.path.endswith(suffix)]

	def read(self, path):
		with open(os.path.join(self.root, path), "rb") as f:
			return f.read()

	def edges_of(self, path, data):
		out = []
		for m in INCLUDE_RE.finditer(data):
			quoted = m.group(2) is not None
			inc = (m.group(2) if quoted else m.group(3)).decode("utf-8", "replace")
			line = data.count(b"\n", 0, m.start()) + 1
			out.append(Edge(path, line, m.start(), m.end(), m.group(1), inc, quoted,
				self.resolve(path, inc, quoted)))
		return out

	def all_edges(self):
		if self._edges is None:
			self._edges = []
			for p in self.sources:
				self._edges += self.edges_of(p, self.read(p))
		return self._edges

	# ---- selection -------------------------------------------------------
	def expand(self, names):
		out = []
		for n in names:
			for item in self.groups.get(n, [n]):
				if item not in self.comps:
					raise ToolError(f"unknown component or group: {item}")
				if item not in out:
					out.append(item)
		if not out:
			raise ToolError("no components given")
		return out


# ---- plan -----------------------------------------------------------------
class Plan:
	def __init__(self):
		self.moves = []            # (old, new)
		self.changes = {}          # path -> [(line, old_text, new_text)]
		self.new_data = {}         # path -> rewritten bytes
		self.ambiguities = []      # (path, line, inc, [candidate paths])
		self.warnings = []         # strings
		self.errors = []           # strings

	@property
	def change_count(self):
		return sum(len(v) for v in self.changes.values())


def build_plan(repo, selected):
	plan = Plan()
	sel = set(selected)
	targets = {}
	for h in repo.headers.values():
		if h.comp in sel and h.public and repo.comps[h.comp]["dest"]:
			targets[h.path] = h
			if not h.moved:
				plan.moves.append((h.path, repo.new_path(h)))
	seen = collections.defaultdict(list)
	for old, new in plan.moves:
		seen[new].append(old)
	for new, olds in seen.items():
		if len(olds) > 1 or new in repo.headers:
			plan.errors.append(f"destination collision: {new} <- {olds}")
	for p in repo.sources:
		data = repo.read(p)
		edits = []
		for e in repo.edges_of(p, data):
			hit = [c for c in e.cands if c.path in targets]
			if not hit:
				continue
			if len(e.cands) > 1:
				plan.ambiguities.append((p, e.line, e.inc, [c.path for c in e.cands]))
				continue
			new_inc = hit[0].canonical
			if e.quoted or e.inc != new_inc:  # already `<snk/...>` canonical: nothing to do
				old_text = data[e.start:e.end].decode("utf-8", "replace").strip()
				edits.append((e, new_inc, old_text))
		if edits:
			plan.changes[p] = [(e.line, old, f"#include <{new}>") for e, new, old in edits]
			plan.new_data[p] = rewrite(data, edits)
	plan.warnings += outside_use_warnings(repo, sel)
	return plan


def rewrite(data, edits):
	out, pos = [], 0
	for e, new_inc, _ in edits:
		out.append(data[pos:e.start])
		out.append(e.prefix + b"<" + new_inc.encode() + b">")
		pos = e.end
	out.append(data[pos:])
	return b"".join(out)


def outside_use_warnings(repo, sel):
	"""Internal headers of the selected components that other components include."""
	uses = collections.defaultdict(set)
	for e in repo.all_edges():
		t = e.target
		if t and t.comp in sel and t.internal:
			who = repo.owner(e.path)
			if who != t.comp:
				uses[t.path].add(f"{e.path}:{e.line}")
	return [f"internal {h} is included outside its component by {len(w)} file(s), e.g. {sorted(w)[0]}"
		for h, w in sorted(uses.items())]


def file_eol_kind(data):
	crlf, lf = b"\r\n" in data, bool(LONE_LF_RE.search(data))
	return "mixed" if crlf and lf else "CRLF" if crlf else "LF"


def eol_counts(repo, paths):
	return collections.Counter(file_eol_kind(repo.read(p)) for p in paths)


def print_plan(repo, selected, plan, out=None):
	w = lambda s="": print(s, file=out or sys.stdout)
	w(f"# plan: {' '.join(selected)}")
	w(f"## git mv ({len(plan.moves)})")
	for old, new in plan.moves:
		w(f"{old} -> {new}")
	w(f"## include changes ({plan.change_count} lines in {len(plan.changes)} files)")
	for p, items in sorted(plan.changes.items()):
		for line, old, new in items:
			w(f"{p}:{line}: {old} -> {new}")
	eols = eol_counts(repo, plan.changes)
	w(f"## touched files by EOL: {dict(eols)}")
	w(f"## ambiguities ({len(plan.ambiguities)})")
	for p, line, inc, cands in plan.ambiguities:
		w(f"{p}:{line}: {inc!r} matches {cands}")
	w(f"## warnings ({len(plan.warnings) + len(plan.errors)})")
	for s in plan.errors:
		w(f"ERROR: {s}")
	for s in plan.warnings:
		w(f"WARN: {s}")


# ---- apply -----------------------------------------------------------------
def git(repo, *args):
	r = subprocess.run(["git", *args], cwd=repo.root, capture_output=True, text=True)
	if r.returncode:
		raise ToolError(f"git {' '.join(args)} failed: {r.stderr.strip()}")


def apply_plan(repo, plan):
	if plan.ambiguities or plan.errors:
		raise ToolError(f"refusing to apply: {len(plan.ambiguities)} ambiguities, {len(plan.errors)} errors "
			"(run `plan` for details)")
	for p, data in plan.new_data.items():
		with open(os.path.join(repo.root, p), "wb") as f:
			f.write(data)
	for old, new in plan.moves:
		os.makedirs(os.path.dirname(os.path.join(repo.root, new)), exist_ok=True)
		git(repo, "mv", old, new)


# ---- check -----------------------------------------------------------------
def find_collisions(repo):
	return {b: [h.path for h in hs] for b, hs in sorted(repo.by_base.items()) if len(hs) > 1}


def find_violations(repo):
	"""Public header -> internal header includes: (pub path, line, inc, internal path)."""
	out = []
	for e in repo.all_edges():
		src = repo.headers.get(e.path)
		if src and src.public and e.target and e.target.internal:
			out.append((e.path, e.line, e.inc, e.target.path))
	return out


def find_remaining(repo):
	"""Non-canonical includes of headers that already moved (check c)."""
	out = []
	for e in repo.all_edges():
		for c in e.cands:
			if c.moved and e.inc != c.canonical:
				out.append((e.path, e.line, e.inc, c.canonical))
	return out


def find_unclassified(repo):
	return sorted(h.path for h in repo.headers.values()
		if h.comp is None and repo.owner(h.path) != EXTERNAL)


def find_ambiguous(repo):
	return [(e.path, e.line, e.inc, [c.path for c in e.cands]) for e in repo.all_edges() if len(e.cands) > 1]


def run_check(repo, out=None):
	w = lambda s="": print(s, file=out or sys.stdout)
	col, vio = find_collisions(repo), find_violations(repo)
	rem, unc, amb = find_remaining(repo), find_unclassified(repo), find_ambiguous(repo)
	w(f"(a) basename collisions: {len(col)}")
	for b, ps in col.items():
		w(f"  {b}: {ps}")
	w(f"(b) public -> internal includes: {len(vio)}")
	for p, line, inc, tgt in vio:
		w(f"  {p}:{line}: {inc} -> internal {tgt}")
	w(f"(c) remaining non-snk includes of moved headers: {len(rem)}")
	for p, line, inc, canon in rem:
		w(f"  {p}:{line}: {inc!r} should be <{canon}>")
	w(f"unclassified headers inside component roots: {len(unc)}")
	for p in unc:
		w(f"  {p}")
	w(f"ambiguous includes: {len(amb)}")
	for p, line, inc, cands in amb:
		w(f"  {p}:{line}: {inc!r} -> {cands}")
	return 1 if (col or vio or rem or unc) else 0


# ---- report ------------------------------------------------------------------
def run_report(repo, out=None):
	w = lambda s="": print(s, file=out or sys.stdout)
	w("# Header split dry run (issue #109)")
	w()
	w("Generated by `python3 tools/header_split/split.py report` on the current tree "
	  "(no files moved). Every group is planned independently against the unmodified tree.")
	w()
	for group, comps in repo.groups.items():
		plan = build_plan(repo, repo.expand([group]))
		eols = eol_counts(repo, plan.changes)
		internal = sum(len(repo.comps[c]["internal"]) for c in comps)
		w(f"## {group}: {', '.join(comps)}")
		w(f"- files moved: {len(plan.moves)} (internal, staying: {internal})")
		w(f"- include lines changed: {plan.change_count} in {len(plan.changes)} files")
		w(f"- touched files by EOL: CRLF {eols['CRLF']}, LF {eols['LF']}, mixed {eols['mixed']}")
		per = collections.Counter(top_dir(p) for p in plan.changes)
		w(f"- touched files by area: {', '.join(f'{k} {v}' for k, v in sorted(per.items()))}")
		w(f"- ambiguities: {len(plan.ambiguities)}" + "".join(f"\n  - {a[0]}:{a[1]} {a[2]!r} -> {a[3]}" for a in plan.ambiguities[:5]))
		w(f"- plan errors: {len(plan.errors)}" + "".join(f"\n  - {s}" for s in plan.errors))
		w(f"- warnings (internal header used outside its component): {len(plan.warnings)}")
		for s in plan.warnings[:6]:
			w(f"  - {s}")
		if len(plan.warnings) > 6:
			w(f"  - ... {len(plan.warnings) - 6} more")
		w()
	w("## check on the current tree")
	col, vio = find_collisions(repo), find_violations(repo)
	w(f"- (a) basename collisions: {len(col)}" + "".join(f"\n  - {b}: {ps}" for b, ps in col.items()))
	w(f"- (c) remaining quoted includes of moved headers: {len(find_remaining(repo))}")
	unc = find_unclassified(repo)
	w(f"- unclassified headers inside component roots: {len(unc)}" + "".join(f"\n  - {p}" for p in unc))
	amb = find_ambiguous(repo)
	w(f"- ambiguous includes anywhere: {len(amb)}")
	pairs = collections.defaultdict(list)
	for p, line, _, tgt in vio:
		pairs[(p, tgt)].append(line)
	w(f"- (b) public -> internal includes: {len(vio)} lines, {len(pairs)} distinct pairs")
	for (p, tgt), lines in sorted(pairs.items()):
		w(f"  - {short(repo, p)} -> {short(repo, tgt)} (line {', '.join(map(str, lines))})")
	return 0


def top_dir(path):
	parts = path.split("/")
	return "/".join(parts[:2]) if parts[0] == "core" else parts[0]


def short(repo, path):
	h = repo.headers[path]
	return f"{h.comp}:{h.sub}"


# ---- cli ---------------------------------------------------------------------
def default_root():
	return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main(argv=None):
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--root", default=default_root(), help="repository root (default: this checkout)")
	ap.add_argument("--config", help="components.json (default: next to this script)")
	sub = ap.add_subparsers(dest="cmd", required=True)
	for name in ("plan", "apply"):
		p = sub.add_parser(name)
		p.add_argument("components", nargs="+", help="component names or PR-N groups")
	sub.add_parser("check")
	sub.add_parser("report")
	args = ap.parse_args(argv)
	config = args.config or os.path.join(os.path.dirname(os.path.abspath(__file__)), "components.json")
	try:
		repo = Repo(args.root, config)
		if args.cmd == "check":
			return run_check(repo)
		if args.cmd == "report":
			return run_report(repo)
		selected = repo.expand(args.components)
		plan = build_plan(repo, selected)
		if args.cmd == "plan":
			print_plan(repo, selected, plan)
			return 0
		apply_plan(repo, plan)
		print(f"moved {len(plan.moves)} headers, rewrote {plan.change_count} include lines in {len(plan.changes)} files")
		left = find_remaining(Repo(args.root, config))
		for p, line, inc, canon in left:
			print(f"remaining: {p}:{line}: {inc!r} should be <{canon}>")
		return 1 if left else 0
	except ToolError as ex:
		print(f"error: {ex}", file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())
