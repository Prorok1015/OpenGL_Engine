#include "rnd_opaque_pass.h"
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/driver/rnd_ssbo_buffer_interface.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/common/timer.hpp>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"
#include <algorithm>

namespace rnd {
void opaque_pass::execute(frame_context &context,
                          driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Opaque");
  PROFILE_GPU_SCOPE("Pass.Opaque");
  if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>())
    return;

  auto &packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

  auto &txm_manager = *context.textures;
  auto &geom_manager = *context.geometry;
  rnd::global_params common_matrix;

  for (const auto& packet : packets) {
    context.shaders->update_global_sun(
        packet.lights.to_gpu_params());
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    const auto target = rnd::select_camera_target(packet.camera);
    const bool offscreen = target.kind == rnd::render_target_kind::texture;
    res::tag target_tag = target.texture;
    res::tag depth_tag = rnd::resolve_depth_target(packet.camera);
    driver::texture_interface *color_rt = nullptr;
    if (offscreen) {
      color_rt = rnd::render_targets::ensure(
          txm_manager, target_tag, {vp_width, vp_height},
          rnd::driver::texture_header::TYPE::RGBA8,
          rnd::driver::texture_usage::color_target | rnd::driver::texture_usage::sampled);
    }

    rnd::driver::render_state state;
    state.depth.enabled = true;
    state.depth.test_func = rnd::driver::depth_state::func::LEQUAL;
    // The backbuffer has no z-prepass depth attached: this pass writes the depth the later
    // passes (skybox) test against.
    state.depth.write_mask = !offscreen;
    drv.set_render_state(state);

    if (offscreen) {
      drv.push_frame_buffer();
      drv.set_render_target(color_rt, txm_manager.find(depth_tag));
    } else {
      // Several cameras share the backbuffer: clear only this camera's rectangle.
      drv.set_scissor(glm::ivec4{packet.camera.viewport.x,
                                 packet.camera.viewport.y, vp_width, vp_height});
      drv.clear(rnd::driver::CLEAR_FLAGS::DEPTH_BUFFER);
    }
    drv.clear(rnd::driver::CLEAR_FLAGS::COLOR_BUFFER, {glm::vec4(0)});
    if (!offscreen) {
      drv.set_scissor(glm::ivec4{0});
    }

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    context.shaders->update_global_uniform(common_matrix);
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    for (const auto &dc : packet.opaque_draws) {
      auto *va = geom_manager.require_geometry(dc.geometry_tag);
      if (!va)
        continue;

      rnd::configure_pass(context, dc.variant, *dc.material, &dc.transform);
      if (dc.bone_matrices && !dc.bone_matrices->empty() && dc.skinning_tag.is_valid()) {
          if (auto* skm = context.skinning) {
              skm->bind_skin(&drv, dc.skinning_tag, *dc.bone_matrices);
          }
      }

      drv.draw_indices(va, rnd::RENDER_MODE::TRIANGLE, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    if (offscreen) {
      drv.pop_frame_buffer();
    }
  }
}
} // namespace rnd
