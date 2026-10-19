#include "rnd_opaque_pass.h"
#include <snk/render/rnd_light_data.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_context.h>
#include "rnd_pass_pipelines.h"
#include <snk/common/timer.hpp>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"
#include <algorithm>

namespace rnd {
void opaque_pass::execute(render_context &context,
                          driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Opaque");
  PROFILE_GPU_SCOPE("Pass.Opaque");
  const auto packets = context.packets();
  if (packets.empty())
    return;

  rnd::global_params common_matrix;

  for (const auto& packet : packets) {
    context.write_lights(
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
      color_rt = context.ensure_render_target(target_tag, {vp_width, vp_height},
          rnd::driver::texture_header::TYPE::RGBA8,
          rnd::driver::texture_usage::color_target | rnd::driver::texture_usage::sampled);
    }

    // The camera's clear mode: a lower layer's colour survives under an overlay that clears only the depth.
    const auto color_load = rnd::clears_color(packet.camera.clear)
        ? rnd::driver::load_op::clear : rnd::driver::load_op::load;
    const auto depth_load = rnd::clears_depth(packet.camera.clear)
        ? rnd::driver::load_op::clear : rnd::driver::load_op::load;

    rnd::driver::render_pass_desc pass;
    pass.debug_name = "opaque";
    if (offscreen) {
      // The depth of an offscreen camera is cleared by the z-prepass.
      pass.colors.push_back({.texture = color_rt, .load = color_load});
      if (auto *depth = context.find_texture(depth_tag)) {
        pass.depth_stencil = rnd::driver::attachment_desc{.texture = depth};
      }
    } else {
      // Several cameras share the backbuffer: clear only this camera's rectangle.
      pass.backbuffer = true;
      pass.colors.push_back({.load = color_load});
      pass.depth_stencil = rnd::driver::attachment_desc{.load = depth_load};
      pass.render_area = glm::ivec4{packet.camera.viewport.x,
                                    packet.camera.viewport.y, vp_width, vp_height};
    }
    drv.begin_render_pass(pass);

    rnd::driver::pipeline_desc state;
    state.depth.enabled = true;
    state.depth.test_func = rnd::driver::depth_state::func::LEQUAL;
    // The backbuffer has no z-prepass depth attached: this pass writes the depth the later
    // passes (skybox) test against.
    state.depth.write_mask = !offscreen;
    rnd::driver::set_target_formats(state, pass);
    state.set_layouts = context.layouts().scene();
    rnd::pass_pipelines pipelines(drv, context, state);

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    context.write_view(common_matrix);
    context.bind_frame();
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    for (std::size_t i = 0; i < packet.opaque_draws.size(); ++i) {
      const auto &dc = packet.opaque_draws[i];
      auto *va = context.geometry(packet.geometry_of(dc));
      if (!va)
        continue;

      if (!pipelines.bind(dc.variant))
        continue;
      rnd::configure_pass(context, packet.material_of(dc));
      context.bind_object(packet.opaque_object(i), packet.skin_of(dc));

      drv.draw_indices(va, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    drv.end_render_pass();
  }
}
} // namespace rnd
