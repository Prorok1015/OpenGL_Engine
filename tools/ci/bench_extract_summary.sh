#!/usr/bin/env bash
# Prints the extract benchmark report (tests/app_engine_null_backend_bench.cpp) as a Markdown table.
#
#   tools/ci/bench_extract_summary.sh bench_extract.json >> "$GITHUB_STEP_SUMMARY"
#
# Needs jq.
set -euo pipefail

report="${1:?bench_extract.json}"

jq -r '
	"### Extract benchmark (null backend, \(.build) build)",
	"",
	"\(.meshes) meshes, \(.cameras) cameras, \(.frames) frames after \(.warmup_frames) warm-up; per-frame totals in ms.",
	"",
	"| zone | mean | p50 | p95 | max | calls/frame |",
	"|---|---:|---:|---:|---:|---:|",
	(.zones | to_entries[] | "| \(.key) | \(.value.mean_ms * 1000 | round / 1000) | \(.value.p50_ms * 1000 | round / 1000) | \(.value.p95_ms * 1000 | round / 1000) | \(.value.max_ms * 1000 | round / 1000) | \(.value.calls_per_frame) |")
' "$report"
