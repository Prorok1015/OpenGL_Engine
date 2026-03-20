#include "rnd_transparent_pass.h"
#include "rnd_light_data.hpp"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "rnd_ssbo_buffer_interface.h"
#include "skinning/rnd_skinning_manager.h"
#include "timer.hpp"
#include "eng_profiler.h"
#include "rnd_gpu_profiler.h"

namespace rnd {
void transparent_pass::execute(frame_context &context,
                               driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Transparent");
  PROFILE_GPU_SCOPE("Pass.Transparent");

  if (!context.data.has_value<std::vector<rnd::render_packet_t>>()) {
    return;
  }
  auto &packets = context.data.require<std::vector<rnd::render_packet_t>>();

  if (context.data.has_value<rnd::scene_lights_t>()) {
    auto &scene_lights = context.data.require<rnd::scene_lights_t>();
    rnd::get_system().get_shader_manager().update_global_sun(
        scene_lights.to_gpu_params());
  }

  static res::tag z_pass_tag = res::tag(res::tag::memory, "__z_prepass_rt");
  static res::tag color_rt_transparent_tag =
      res::tag(res::tag::memory, "__color_rt_transparent_rt");
  static res::tag waight_rt_transparent_tag =
      res::tag(res::tag::memory, "__waight_rt_transparent_rt");

  auto &txm_manager = rnd::get_system().get_texture_manager();
  auto &geom_manager = rnd::get_system().get_geom_manager();
  rnd::global_params common_matrix;

  for (auto packet : packets) {
    if (packet.transparent_draws.empty())
      continue;

    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    auto color_tp_rt = txm_manager.find(color_rt_transparent_tag);
    auto waight_tp_rt = txm_manager.find(waight_rt_transparent_tag);

    if (color_tp_rt && (color_tp_rt->width() != vp_width ||
                        color_tp_rt->height() != vp_height)) {
      txm_manager.remove(color_rt_transparent_tag);
      txm_manager.remove(waight_rt_transparent_tag);
      color_tp_rt = nullptr;
      waight_tp_rt = nullptr;
    }

    if (!color_tp_rt) {
      rnd::driver::texture_header header;
      header.data.extent.width = vp_width;
      header.data.extent.height = vp_height;
      header.data.format = rnd::driver::texture_header::TYPE::RGBA16F;
      header.usage = rnd::driver::TEXTURE_USAGE::COLOR_TARGET;
      header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
      header.mag = rnd::driver::texture_header::FILTERING::LINEAR;
      header.min = rnd::driver::texture_header::FILTERING::LINEAR;
      color_tp_rt =
          txm_manager.generate_texture(color_rt_transparent_tag, header);

      header.data.format = rnd::driver::texture_header::TYPE::R8;
      waight_tp_rt =
          txm_manager.generate_texture(waight_rt_transparent_tag, header);
    }

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
                           txm_manager.find(z_pass_tag));
    drv.clear(rnd::driver::CLEAR_FLAGS::COLOR_BUFFER,
              {glm::vec4(0), glm::vec4(1)});
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    rnd::get_system().get_shader_manager().update_global_uniform(common_matrix);

    for (const auto &dc : packet.transparent_draws) {
      auto *va = geom_manager.require_geometry(dc.geometry_tag);
      if (!va)
        continue;

      rnd::configure_pass(dc.material);
      if (!dc.bone_matrices.empty() && dc.skinning_tag.is_valid()) {
          if (auto* skm = rnd::get_system().get_skinning_manager()) {
              skm->bind_skin(&drv, dc.skinning_tag, dc.bone_matrices);
          }
      }

      drv.draw_indices(va, rnd::RENDER_MODE::TRIANGLE, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    drv.pop_frame_buffer();
  }
}
} // namespace rnd
