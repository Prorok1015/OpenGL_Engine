#include "rnd_transparent_pass.h"
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_context.h>
#include "rnd_pass_pipelines.h"
#include <snk/common/timer.hpp>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
void transparent_pass::execute(render_context &context,
                               driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Transparent");
  PROFILE_GPU_SCOPE("Pass.Transparent");

  const auto packets = context.packets();
  if (packets.empty()) {
    return;
  }

  rnd::global_params common_matrix;

  for (const auto& packet : packets) {
    context.write_lights(
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
    auto color_tp_rt = context.ensure_render_target(tp_accum_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::RGBA16F, tp_usage, tp_sampling);
    auto waight_tp_rt = context.ensure_render_target(tp_reveal_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::R8, tp_usage, tp_sampling);

    rnd::driver::pipeline_desc state;
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

    state.blend = {color_blend, weight_blend};

    rnd::driver::render_pass_desc pass;
    pass.colors.push_back({.texture = color_tp_rt,
                           .load = rnd::driver::load_op::clear,
                           .clear_color = glm::vec4(0)});
    pass.colors.push_back({.texture = waight_tp_rt,
                           .load = rnd::driver::load_op::clear,
                           .clear_color = glm::vec4(1)});
    if (auto *depth = context.find_texture(depth_tag)) {
      pass.depth_stencil = rnd::driver::attachment_desc{.texture = depth};
    }
    pass.debug_name = "transparent";
    drv.begin_render_pass(pass);
    rnd::driver::set_target_formats(state, pass);
    state.set_layouts = context.layouts().scene();
    rnd::pass_pipelines pipelines(drv, context, state);
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    context.write_view(common_matrix);
    context.bind_frame();

    for (std::size_t i = 0; i < packet.transparent_draws.size(); ++i) {
      const auto &dc = packet.transparent_draws[i];
      auto *va = context.geometry(packet.geometry_of(dc));
      if (!va)
        continue;

      if (!pipelines.bind(context.material(dc.material).base))
        continue;
      rnd::configure_pass(context, dc.material);
      context.bind_object(packet.transparent_object(i), packet.skin_of(dc));

      drv.draw_indices(va, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    drv.end_render_pass();
  }
}
} // namespace rnd
