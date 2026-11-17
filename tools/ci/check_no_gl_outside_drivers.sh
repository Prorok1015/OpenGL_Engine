#!/usr/bin/env bash
# Checks that OpenGL is used only inside the GL driver (RHI step R0, docs/roadmap/design/RHI.md section 5).
#
#   tools/ci/check_no_gl_outside_drivers.sh [repo root]
#
# Scans the C/C++ files git tracks (third-party trees under lib3dparty are skipped) for GL API use:
# gl* calls, GL_* constants, GL* types, glad/GLFW/GL headers, the internal rnd_gl_* driver headers,
# ImGui's OpenGL backend and rnd::driver::gl::. The GL driver directory is the one place they belong.
#
# The only other places GL may appear are the documented exceptions below (R7c, #118; the same list is in
# docs/roadmap/design/RHI.md). An exception names a path AND what is allowed there: a GL hit in that path
# passes only when its line matches the entry's regex, anything else in the path is reported like a hit
# anywhere else. So the GLFW backend may include GLFW, the ImGui backend may use imgui_impl_opengl3, and the
# render init may name the GL swapchain; none of them may start calling GL.
#
# The check is tight in both directions: a hit outside the exceptions fails it, and an exception that no
# longer has a hit fails it too, so a stale exception cannot linger.
#
# A second rule keeps the object behind a texture_handle (texture_object_access, rnd_texture_backend.h,
# texture_object()) inside the drivers and the two driver test files.
set -euo pipefail

DRIVER_DIR='core/core/video/drivers/impls/opengl'

# Parallel arrays: path prefix ('/' at the end covers a directory, otherwise it names one file),
# regex (ERE) of the lines allowed there, reason.
ALLOW_PATH=(
	'core/core/windows/window_service/impl_glfw/'
	'core/engine/gui/backends/opengl/'
	'core/engine/render/render_system/rnd_render_service_init.cpp'
)
ALLOW_RE=(
	'#[[:space:]]*include[[:space:]]*<GLFW/glfw3\.h>'
	'imgui_impl_opengl3|ImGui_ImplOpenGL3_'
	'rnd_gl_swapchain\.h|driver::gl::swapchain'
)
ALLOW_WHY=(
	'GLFW window backend: creates the GL context (window hints), makes it current, swaps buffers; the only GL header it includes is the GLFW one'
	'stock ImGui OpenGL3 renderer (imgui_impl_opengl3), kept by the owner; textures reach it as ids from imgui_backend_interface::to_imgui, it knows no driver class'
	'backend selection (render/backend): only the GL swapchain, next to the null and validation ones'
)

ACCESS_RE='texture_object_access|rnd_texture_backend\.h|\btexture_object\('
ACCESS_ALLOW=(
	'core/core/video/drivers/'
	'tests/rnd_null_driver_tests.cpp'
	'tests/rnd_validation_tests.cpp'
)

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "${root}"

gl_re='(\bgl[A-Z][A-Za-z0-9_]*[[:space:]]*\()'
gl_re+='|(\bGL_[A-Z0-9_]+\b)'
gl_re+='|(\bGL(u?(int|byte|short)|enum|float|boolean|sizei|bitfield|char|sync|sizeiptr|intptr|double|clampf)\b)'
gl_re+='|(\bGLAD_[A-Z0-9_]+)'
gl_re+='|(#[[:space:]]*include[[:space:]]*[<"](glad|GLFW|GL|OpenGL|KHR)/)'
gl_re+='|(#[[:space:]]*include[[:space:]]*[<"]([A-Za-z0-9_./-]*/)?(rnd_gl_[A-Za-z0-9_]+|open_gl_specific)\.h[>"])'
gl_re+='|(imgui_impl_opengl|ImGui_ImplOpenGL)'
gl_re+='|(\bdriver::gl::)'

sources=('*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.inl' '*.ipp' '*.mm')

hits="$(git grep -nIE "${gl_re}" -- "${sources[@]}" \
	":(exclude)lib3dparty" ":(exclude)${DRIVER_DIR}" || true)"

declare -a allow_hits=()
for i in "${!ALLOW_PATH[@]}"; do
	allow_hits[i]=0
done

outside=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	text="${line#*:}"	# 'lineno:content'
	text="${text#*:}"
	matched=-1
	for i in "${!ALLOW_PATH[@]}"; do
		if [[ "${path}" == "${ALLOW_PATH[i]}"* ]]; then
			matched="${i}"
			break
		fi
	done
	if ((matched >= 0)) && [[ "${text}" =~ ${ALLOW_RE[matched]} ]]; then
		allow_hits[matched]=$((allow_hits[matched] + 1))
	else
		outside+=("${line}")
	fi
done <<<"${hits}"

access_hits="$(git grep -nIE "${ACCESS_RE}" -- "${sources[@]}" ":(exclude)lib3dparty" || true)"
declare -a access_count=()
for i in "${!ACCESS_ALLOW[@]}"; do
	access_count[i]=0
done
access_outside=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	matched=-1
	for i in "${!ACCESS_ALLOW[@]}"; do
		if [[ "${path}" == "${ACCESS_ALLOW[i]}"* ]]; then
			matched="${i}"
			break
		fi
	done
	if ((matched >= 0)); then
		access_count[matched]=$((access_count[matched] + 1))
	else
		access_outside+=("${line}")
	fi
done <<<"${access_hits}"

echo "GL outside ${DRIVER_DIR}/:"
for i in "${!ALLOW_PATH[@]}"; do
	printf '  allowed  %3d hit(s)  %s\n           only: %s\n           %s\n' "${allow_hits[i]}" "${ALLOW_PATH[i]}" "${ALLOW_RE[i]}" "${ALLOW_WHY[i]}"
done
echo "texture_object_access allowed in:"
for i in "${!ACCESS_ALLOW[@]}"; do
	printf '  allowed  %3d hit(s)  %s\n' "${access_count[i]}" "${ACCESS_ALLOW[i]}"
done

status=0
if ((${#outside[@]} > 0)); then
	echo "::error::${#outside[@]} GL use(s) outside the GL driver and the allow-list of tools/ci/check_no_gl_outside_drivers.sh"
	for line in "${outside[@]}"; do
		echo "  ${line}"
	done
	status=1
fi
if ((${#access_outside[@]} > 0)); then
	echo "::error::${#access_outside[@]} use(s) of texture_object_access outside the drivers and their tests"
	for line in "${access_outside[@]}"; do
		echo "  ${line}"
	done
	status=1
fi
for i in "${!ALLOW_PATH[@]}"; do
	if ((allow_hits[i] == 0)); then
		echo "::error::allow-list entry '${ALLOW_PATH[i]}' has no GL use left; remove it from tools/ci/check_no_gl_outside_drivers.sh"
		status=1
	fi
done
for i in "${!ACCESS_ALLOW[@]}"; do
	if ((access_count[i] == 0)); then
		echo "::error::texture_object_access entry '${ACCESS_ALLOW[i]}' has no use left; remove it from tools/ci/check_no_gl_outside_drivers.sh"
		status=1
	fi
done
exit "${status}"
