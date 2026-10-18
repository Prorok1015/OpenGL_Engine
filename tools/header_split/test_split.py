import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import split  # noqa: E402

CONFIG = {
	"components": [
		{"name": "alpha", "root": "core/core/alpha", "dest": "snk/alpha",
			"public": ["a_pub.h", "sub/a_deep.h"], "internal": ["a_int.h"]},
		{"name": "beta", "root": "core/core/beta", "dest": "snk/beta",
			"public": ["b_pub.h"], "internal": []},
	],
	"pr_groups": {"PR-X": ["alpha", "beta"]},
}

FILES = {
	# CRLF public header that includes an internal one (violation) and a sibling public one.
	"core/core/alpha/a_pub.h": b'#pragma once\r\n#include "a_int.h"\r\n#include "sub/a_deep.h"\r\nint a();\r\n',
	"core/core/alpha/sub/a_deep.h": b'#pragma once\n#include <vector>\nint deep();\n',
	"core/core/alpha/a_int.h": b'#pragma once\n',
	"core/core/beta/b_pub.h": b'#pragma once\r\n#  include   "a_pub.h"\r\n',
	# LF source, flat + subpath + angle + already-foreign includes.
	"core/core/alpha/a.cpp": b'#include "a_pub.h"\n#include "sub/a_deep.h"\n#include "third/party.h"\nint a() { return 1; }\n',
	# CRLF source with a lone LF in the middle (mixed) and trailing text after the include.
	"Editor/main.cpp": (b'#include "a_pub.h" // keep me\r\n#include <b_pub.h>\r\n'
		b'// #include "a_pub.h"\r\nint x;\nint y;\r\n'),
	"tests/t.cpp": b'#include <a_deep.h>\n#include "a_deep.h"\n',
}


@contextlib.contextmanager
def fixture(extra=None, config=CONFIG):
	with tempfile.TemporaryDirectory() as root:
		for path, data in {**FILES, **(extra or {})}.items():
			full = os.path.join(root, path)
			os.makedirs(os.path.dirname(full), exist_ok=True)
			with open(full, "wb") as f:
				f.write(data)
		cfg = os.path.join(root, "components.json")
		with open(cfg, "w") as f:
			json.dump(config, f)
		subprocess.run(["git", "init", "-q"], cwd=root, check=True)
		subprocess.run(["git", "add", "-A"], cwd=root, check=True)
		subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "init"],
			cwd=root, check=True)
		yield root, cfg


def read(root, path):
	with open(os.path.join(root, path), "rb") as f:
		return f.read()


def run_cli(root, cfg, *args):
	out, err = io.StringIO(), io.StringIO()
	with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
		code = split.main(["--root", root, "--config", cfg, *args])
	return code, out.getvalue(), err.getvalue()


class PlanTest(unittest.TestCase):
	def test_plan_lists_moves_and_changes_without_touching_files(self):
		with fixture() as (root, cfg):
			before = read(root, "Editor/main.cpp")
			code, out, _ = run_cli(root, cfg, "plan", "alpha")
			self.assertEqual(code, 0)
			self.assertIn("core/core/alpha/a_pub.h -> core/core/alpha/include/snk/alpha/a_pub.h", out)
			self.assertIn("core/core/alpha/sub/a_deep.h -> core/core/alpha/include/snk/alpha/sub/a_deep.h", out)
			self.assertNotIn("a_int.h ->", out)  # internal stays
			self.assertIn('Editor/main.cpp:1: #include "a_pub.h" -> #include <snk/alpha/a_pub.h>', out)
			self.assertIn("tests/t.cpp:1: #include <a_deep.h> -> #include <snk/alpha/sub/a_deep.h>", out)
			self.assertEqual(read(root, "Editor/main.cpp"), before)
			self.assertFalse(os.path.exists(os.path.join(root, "core/core/alpha/include")))

	def test_group_expands_to_components(self):
		with fixture() as (root, cfg):
			repo = split.Repo(root, cfg)
			self.assertEqual(repo.expand(["PR-X"]), ["alpha", "beta"])
			with self.assertRaises(split.ToolError):
				repo.expand(["nope"])

	def test_only_selected_components_change(self):
		with fixture() as (root, cfg):
			plan = split.build_plan(split.Repo(root, cfg), ["beta"])
			self.assertEqual([m[0] for m in plan.moves], ["core/core/beta/b_pub.h"])
			self.assertEqual(list(plan.changes), ["Editor/main.cpp"])


class ApplyTest(unittest.TestCase):
	def test_apply_rewrites_bytes_exactly_and_moves(self):
		with fixture() as (root, cfg):
			code, out, _ = run_cli(root, cfg, "apply", "alpha")
			self.assertEqual(code, 0, out)
			base = "core/core/alpha/include/snk/alpha/"
			# CRLF kept, internal include untouched, subpath include rewritten.
			self.assertEqual(read(root, base + "a_pub.h"),
				b'#pragma once\r\n#include "a_int.h"\r\n#include <snk/alpha/sub/a_deep.h>\r\nint a();\r\n')
			self.assertEqual(read(root, base + "sub/a_deep.h"), FILES["core/core/alpha/sub/a_deep.h"])
			# LF source: third-party quoted include left alone.
			self.assertEqual(read(root, "core/core/alpha/a.cpp"),
				b'#include <snk/alpha/a_pub.h>\n#include <snk/alpha/sub/a_deep.h>\n#include "third/party.h"\n'
				b'int a() { return 1; }\n')
			# Mixed EOL file: trailing comment, commented-out include and the lone LF survive.
			self.assertEqual(read(root, "Editor/main.cpp"),
				b'#include <snk/alpha/a_pub.h> // keep me\r\n#include <b_pub.h>\r\n'
				b'// #include "a_pub.h"\r\nint x;\nint y;\r\n')
			# Beta header (CRLF, odd spacing) includes alpha too.
			self.assertEqual(read(root, "core/core/beta/b_pub.h"),
				b'#pragma once\r\n#  include   <snk/alpha/a_pub.h>\r\n')
			self.assertEqual(read(root, "tests/t.cpp"),
				b'#include <snk/alpha/sub/a_deep.h>\n#include <snk/alpha/sub/a_deep.h>\n')
			self.assertTrue(os.path.exists(os.path.join(root, "core/core/alpha/a_int.h")))
			self.assertFalse(os.path.exists(os.path.join(root, "core/core/alpha/a_pub.h")))

	def test_git_sees_renames(self):
		with fixture() as (root, cfg):
			run_cli(root, cfg, "apply", "alpha")
			status = subprocess.run(["git", "status", "--porcelain"], cwd=root, capture_output=True, text=True).stdout
			self.assertRegex(status, r"R[ M] core/core/alpha/a_pub.h -> core/core/alpha/include/snk/alpha/a_pub.h")

	def test_apply_is_idempotent(self):
		with fixture() as (root, cfg):
			run_cli(root, cfg, "apply", "alpha", "beta")
			snapshot = {p: read(root, p) for p in ("Editor/main.cpp", "tests/t.cpp")}
			code, out, _ = run_cli(root, cfg, "plan", "alpha", "beta")
			self.assertIn("(0 lines in 0 files)", out)
			self.assertEqual(run_cli(root, cfg, "apply", "alpha", "beta")[0], 0)
			self.assertEqual({p: read(root, p) for p in snapshot}, snapshot)

	def test_ambiguity_fails_and_leaves_tree_untouched(self):
		extra = {
			"core/core/alpha/x/dup.h": b"#pragma once\n",
			"core/core/beta/y/dup.h": b"#pragma once\n",
			"Editor/uses_dup.cpp": b'#include "dup.h"\n',
			"core/core/alpha/uses_local.cpp": b'#include "x/dup.h"\n',  # unambiguous: suffix is unique
		}
		config = json.loads(json.dumps(CONFIG))
		config["components"][0]["public"].append("x/dup.h")
		config["components"][1]["public"].append("y/dup.h")
		with fixture(extra, config) as (root, cfg):
			before = read(root, "Editor/main.cpp")
			code, out, _ = run_cli(root, cfg, "plan", "alpha")
			self.assertIn("Editor/uses_dup.cpp:1: 'dup.h' matches", out)
			code, _, err = run_cli(root, cfg, "apply", "alpha")
			self.assertEqual(code, 1)
			self.assertIn("ambiguities", err)
			self.assertEqual(read(root, "Editor/main.cpp"), before)
			self.assertTrue(os.path.exists(os.path.join(root, "core/core/alpha/a_pub.h")))

	def test_same_directory_include_wins_over_ambiguity(self):
		extra = {
			"core/core/alpha/x/dup.h": b"#pragma once\n",
			"core/core/beta/y/dup.h": b"#pragma once\n",
			"core/core/alpha/x/user.cpp": b'#include "dup.h"\n',
		}
		config = json.loads(json.dumps(CONFIG))
		config["components"][0]["public"].append("x/dup.h")
		config["components"][1]["public"].append("y/dup.h")
		with fixture(extra, config) as (root, cfg):
			plan = split.build_plan(split.Repo(root, cfg), ["alpha"])
			self.assertEqual(plan.ambiguities, [])
			self.assertIn("core/core/alpha/x/user.cpp", plan.changes)


class CheckTest(unittest.TestCase):
	def test_public_to_internal_violation_is_reported(self):
		with fixture() as (root, cfg):
			code, out, _ = run_cli(root, cfg, "check")
			self.assertEqual(code, 1)
			self.assertIn("(b) public -> internal includes: 1", out)
			self.assertIn("core/core/alpha/a_pub.h:2: a_int.h -> internal core/core/alpha/a_int.h", out)

	def test_clean_tree_passes(self):
		files = {"core/core/alpha/a_pub.h": b"#pragma once\n"}
		with fixture(files) as (root, cfg):
			self.assertEqual(run_cli(root, cfg, "check")[0], 0)

	def test_basename_collision_is_reported(self):
		with fixture({"Editor/b_pub.h": b"#pragma once\n"}) as (root, cfg):
			code, out, _ = run_cli(root, cfg, "check")
			self.assertIn("(a) basename collisions: 1", out)
			self.assertEqual(code, 1)

	def test_remaining_quoted_include_of_moved_header(self):
		with fixture() as (root, cfg):
			run_cli(root, cfg, "apply", "alpha")
			with open(os.path.join(root, "Editor/late.cpp"), "wb") as f:
				f.write(b'#include "sub/a_deep.h"\r\n')
			_, out, _ = run_cli(root, cfg, "check")
			self.assertIn("(c) remaining non-snk includes of moved headers: 1", out)
			self.assertIn("Editor/late.cpp:1: 'sub/a_deep.h' should be <snk/alpha/sub/a_deep.h>", out)

	def test_unclassified_header_is_reported(self):
		with fixture({"core/core/alpha/new_one.h": b"#pragma once\n"}) as (root, cfg):
			_, out, _ = run_cli(root, cfg, "check")
			self.assertIn("unclassified headers inside component roots: 1", out)

	def test_listed_header_missing_is_an_error(self):
		config = json.loads(json.dumps(CONFIG))
		config["components"][1]["public"].append("ghost.h")
		with fixture(config=config) as (root, cfg):
			code, _, err = run_cli(root, cfg, "check")
			self.assertEqual(code, 1)
			self.assertIn("ghost.h", err)


if __name__ == "__main__":
	unittest.main()
