#!/usr/bin/env bash
# Checks that OpenGL is used only inside the GL driver (RHI step R0, docs/roadmap/design/RHI.md section 5).
#
#   tools/ci/check_no_gl_outside_drivers.sh [repo root]
#
# Scans the C/C++ files git tracks (third-party trees under lib3dparty are skipped) for GL API use:
# gl* calls, GL_* constants, GL* types, glad/GLFW/GL headers, the internal rnd_gl_* driver headers,
# ImGui's OpenGL backend and rnd::driver::gl::. The GL driver directory is the one place they belong.
# Everything else that exists today is in ALLOW below, with the reason; the strict criterion "no GL
# outside drv_*" is R7 (#118) and each entry is debt that R7 removes.
#
# The allow-list is tight in both directions: a hit outside it fails the check, and an entry that no
# longer has a hit fails it too, so the list shrinks as the debt is paid.
set -euo pipefail

DRIVER_DIR='core/core/video/drivers/impls/opengl'

# 'path prefix|reason'. A prefix ending in '/' covers a directory, otherwise it names one file.
ALLOW=(
	'core/core/windows/window_service/impl_glfw/|GLFW window backend: creates the GL context (window hints), makes it current, swaps buffers, loads glad through glfwGetProcAddress'
	'core/engine/gui/backends/opengl/|ImGui OpenGL3 renderer backend: imgui_impl_opengl3 plus the gl::texture id lookup; replaced by gui::renderer_backend::to_imgui in R7'
	'core/engine/render/render_system/rnd_render_service_init.cpp|backend selection (render/backend): constructs rnd::driver::gl::swapchain next to the null and validation ones'
)

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "${root}"

gl_re='(\bgl[A-Z][A-Za-z0-9_]*[[:space:]]*\()'
gl_re+='|(\bGL_[A-Z0-9_]+\b)'
gl_re+='|(\bGL(u?(int|byte|short)|enum|float|boolean|sizei|bitfield|char|sync|sizeiptr|intptr|double|clampf)\b)'
gl_re+='|(\bGLAD_[A-Z0-9_]+)'
gl_re+='|(#[[:space:]]*include[[:space:]]*[<"](glad|GLFW|GL|OpenGL|KHR)/)'
gl_re+='|(#[[:space:]]*include[[:space:]]*[<"](rnd_gl_[A-Za-z0-9_]+|open_gl_specific)\.h[>"])'
gl_re+='|(imgui_impl_opengl|ImGui_ImplOpenGL)'
gl_re+='|(\bdriver::gl::)'

hits="$(git grep -nIE "${gl_re}" -- \
	'*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.inl' '*.ipp' '*.mm' \
	":(exclude)lib3dparty" ":(exclude)${DRIVER_DIR}" || true)"

declare -A allow_hits=()
for entry in "${ALLOW[@]}"; do
	allow_hits["${entry%%|*}"]=0
done

outside=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	matched=''
	for prefix in "${!allow_hits[@]}"; do
		if [[ "${path}" == "${prefix}"* ]]; then
			matched="${prefix}"
			break
		fi
	done
	if [[ -n "${matched}" ]]; then
		allow_hits["${matched}"]=$((allow_hits["${matched}"] + 1))
	else
		outside+=("${line}")
	fi
done <<<"${hits}"

echo "GL outside ${DRIVER_DIR}/:"
for entry in "${ALLOW[@]}"; do
	prefix="${entry%%|*}"
	printf '  allowed  %3d hit(s)  %s\n           %s\n' "${allow_hits[${prefix}]}" "${prefix}" "${entry#*|}"
done

status=0
if ((${#outside[@]} > 0)); then
	echo "::error::${#outside[@]} GL use(s) outside the GL driver and the allow-list of tools/ci/check_no_gl_outside_drivers.sh"
	for line in "${outside[@]}"; do
		echo "  ${line}"
	done
	status=1
fi
for entry in "${ALLOW[@]}"; do
	prefix="${entry%%|*}"
	if ((allow_hits[${prefix}] == 0)); then
		echo "::error::allow-list entry '${prefix}' has no GL use left; remove it from tools/ci/check_no_gl_outside_drivers.sh"
		status=1
	fi
done
exit "${status}"
