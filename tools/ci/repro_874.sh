#!/usr/bin/env bash
# #874 repro only (wip/874-repro, never merged): desc_resave linked with snk::project_migration, run as the migration
# step ran it; then a backtrace and the boost.json symbols the executable and snk_engine both define.
# Usage: repro_874.sh <build dir> [gdb]
set -u
build="$1"
use_gdb="${2:-}"
tool="$build/tools/desc_resave/desc_resave"
engine="$(find "$build" -name 'libsnk_engine*.so*' -type f | head -1)"
work="${RUNNER_TEMP:-/tmp}/repro_874"
rm -rf "$work"; mkdir -p "$work"
cp -r tests/data/projects/migration_v1 "$work/fx"

echo "::group::run (migration_v1 res, --base core/res)"
rc=0; "$tool" "$work/fx/res" --base core/res --check || rc=$?
echo "exit code: $rc"
echo "::endgroup::"
echo "::group::run (core/res)"
rc2=0; "$tool" core/res --check || rc2=$?
echo "exit code: $rc2"
echo "::endgroup::"

if [ -n "$use_gdb" ]; then
	echo "::group::gdb backtrace"
	gdb -q -batch -ex run -ex 'thread apply all bt 30' -ex 'info sharedlibrary' --args "$tool" "$work/fx/res" --base core/res --check 2>&1 | tail -200
	echo "::endgroup::"
fi

echo "engine: $engine"
echo "::group::boost::json defined in both (demangled)"
nm -C --defined-only "$tool" | grep 'boost::json' | sed -E 's/^[0-9a-f]+ //' | sort -u > "$work/exe.txt"
nm -C --defined-only "$engine" | grep 'boost::json' | sed -E 's/^[0-9a-f]+ //' | sort -u > "$work/so.txt"
echo "exe: $(wc -l < "$work/exe.txt") boost::json symbols, engine: $(wc -l < "$work/so.txt")"
cut -d' ' -f2- "$work/exe.txt" | sort -u > "$work/exe_names.txt"
cut -d' ' -f2- "$work/so.txt" | sort -u > "$work/so_names.txt"
comm -12 "$work/exe_names.txt" "$work/so_names.txt" > "$work/both.txt"
echo "defined in both: $(wc -l < "$work/both.txt")"
grep -E 'resource|storage_ptr|static|guard|instance|default' "$work/both.txt" | head -60
echo "--- first 60 of all ---"
head -60 "$work/both.txt"
echo "::endgroup::"
echo "::group::exe boost::json data/bss (statics in the executable)"
grep -E '^[bBdDuV] ' "$work/exe.txt" | head -60
echo "::endgroup::"
echo "::group::engine exports boost::json (dynamic table)"
readelf --dyn-syms -W "$engine" | c++filt | grep -c 'boost::json' || true
readelf --dyn-syms -W "$engine" | c++filt | grep 'boost::json' | grep -E 'OBJECT|resource|storage' | head -40
echo "::endgroup::"
echo "::group::exe dynamic boost::json"
readelf --dyn-syms -W "$tool" | c++filt | grep 'boost::json' | head -40
echo "::endgroup::"
echo "::group::ldd"
ldd "$tool"
echo "::endgroup::"
echo "summary: migration_v1 exit $rc, core/res exit $rc2"
if [ "$rc" -eq 139 ] || [ "$rc2" -eq 139 ]; then echo "::error::desc_resave crashed at exit (#874)"; exit 1; fi
