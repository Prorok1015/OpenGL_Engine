#!/usr/bin/env python3
"""Turn a gcovr --json-summary file into a per-area Markdown table (line / branch coverage).

Usage: coverage_summary.py summary.json [--title TEXT]

Areas are the first path components that identify a module, e.g.
  core/core/resource, core/engine/scene, Editor/code/editor_system
so the table shows where coverage is thin instead of one global number.
"""
import argparse
import json
import sys
from collections import defaultdict


def area_of(path: str) -> str:
    parts = path.replace("\\", "/").split("/")
    if parts[0] == "core" and len(parts) > 3:
        return "/".join(parts[:3])
    if parts[0] == "Editor" and len(parts) > 3:
        return "/".join(parts[:3])
    return "/".join(parts[:-1]) or parts[0]


def pct(covered: int, total: int) -> str:
    return "n/a" if total == 0 else f"{100.0 * covered / total:.1f}%"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("summary")
    ap.add_argument("--title", default="Unit-test coverage")
    args = ap.parse_args()

    with open(args.summary, encoding="utf-8") as f:
        data = json.load(f)

    areas = defaultdict(lambda: {"lc": 0, "lt": 0, "bc": 0, "bt": 0})
    for entry in data.get("files", []):
        a = areas[area_of(entry["filename"])]
        a["lc"] += entry.get("line_covered", 0)
        a["lt"] += entry.get("line_total", 0)
        a["bc"] += entry.get("branch_covered", 0)
        a["bt"] += entry.get("branch_total", 0)

    print(f"### {args.title}\n")
    print("| Area | Lines | Line cov | Branch cov |")
    print("|---|---:|---:|---:|")
    for name in sorted(areas):
        a = areas[name]
        print(f"| `{name}` | {a['lc']}/{a['lt']} | {pct(a['lc'], a['lt'])} | {pct(a['bc'], a['bt'])} |")
    print(
        f"| **total** | **{data.get('line_covered', 0)}/{data.get('line_total', 0)}** "
        f"| **{pct(data.get('line_covered', 0), data.get('line_total', 0))}** "
        f"| **{pct(data.get('branch_covered', 0), data.get('branch_total', 0))}** |"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
