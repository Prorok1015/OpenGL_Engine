#include "rnd_transparent_pass.h"
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/driver/rnd_ssbo_buffer_interface.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/common/timer.hpp>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
void transparent_pass::execute(frame_context &context,
                               driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Transparent");
  PROFILE_GPU_SCOPE("Pass.Transparent");

  if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
    return;
  }
  auto &packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

  auto &txm_manager = *context.textures;
  auto &geom_manager = *context.geometry;
  rnd::global_params common_matrix;

  for (const auto& packet : packets) {
    context.shaders->update_global_sun(
        packet.lights.to_gpu_params());
    if (packet.transparent_draws.empty())
      continue;

    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    res::tag depth_tag = rnd::resolve_depth_target(packet.camera);
    res::tag tp_accum_tag = rnd::resolve_tp_accum_target(packet.camera);
    res::tag tp_reveal_tag = rnd::resolve_tp_reveal_target(packet.camera);

    constexpr auto tp_usage = rnd::driver::texture_usage::color_target | rnd::driver::texture_usage::sampled;
    constexpr rnd::render_targets::sampling tp_sampling{
        .min = rnd::driver::texture_header::FILTERING::LINEAR,
        .mag = rnd::driver::texture_header::FILTERING::LINEAR,
        .wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE};
    auto color_tp_rt = rnd::render_targets::ensure(
        txm_manager, tp_accum_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::RGBA16F, tp_usage, tp_sampling);
    auto waight_tp_rt = rnd::render_targets::ensure(
        txm_manager, tp_reveal_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::R8, tp_usage, tp_sampling);

    rnd::driver::render_state state;
    state.depth.enabled = true;
    state.depth.write_mask = false;
    state.depth.test_func = rnd::driver::depth_state::func::LEQUAL;

    rnd::driver::blend_state color_blend;
    color_blend.enabled = true;
    color_blend.src_factor = rnd::driver::blend_state::factor::ONE;
    color_blend.dst_factor = rnd::driver::blend_state::factor::ONE;

    rnd::driver::blend_state weight_blend;
    weight_blend.enabled = true;
    weight_blend.src_factor = rnd::driver::blend_state::factor::ZERO;
    weight_blend.dst_factor =
        rnd::driver::blend_state::factor::ONE_MINUS_SRC_COLOR;

    state.blend_states = {color_blend, weight_blend};
    drv.set_render_state(state);

    drv.push_frame_buffer();
    drv.set_render_targets({color_tp_rt, waight_tp_rt},
                           txm_manager.find(depth_tag));
    drv.clear(rnd::driver::CLEAR_FLAGS::COLOR_BUFFER,
              {glm::vec4(0), glm::vec4(1)});
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    context.shaders->update_global_uniform(common_matrix);

    for (const auto &dc : packet.transparent_draws) {
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

    drv.pop_frame_buffer();
  }
}
} // namespace rnd
