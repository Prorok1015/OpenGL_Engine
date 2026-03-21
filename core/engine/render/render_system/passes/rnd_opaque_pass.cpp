#include "rnd_opaque_pass.h"
#include "rnd_light_data.hpp"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "rnd_ssbo_buffer_interface.h"
#include "skinning/rnd_skinning_manager.h"
#include "timer.hpp"
#include "eng_profiler.h"
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
  if (context.data.has_value<rnd::scene_lights_t>()) {
    rnd::get_system().get_shader_manager().update_global_sun(
        context.data.require<rnd::scene_lights_t>().to_gpu_params());
  }

  static res::tag color_rt_tag = res::tag(res::tag::memory, "__color_scene_rt");
  static res::tag z_pass_tag = res::tag(res::tag::memory, "__z_prepass_rt");

  auto &txm_manager = rnd::get_system().get_texture_manager();
  auto &geom_manager = rnd::get_system().get_geom_manager();
  rnd::global_params common_matrix;

  for (const auto& packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    res::tag target_tag = packet.camera.target_texture_tag;
    if (!target_tag.is_valid()) {
      target_tag = color_rt_tag;
    }
    auto color_rt = txm_manager.find(target_tag);

    if (color_rt &&
        (color_rt->width() != vp_width || color_rt->height() != vp_height)) {
      txm_manager.remove(target_tag);
      color_rt = nullptr;
    }

    if (!color_rt) {
      rnd::driver::texture_header header;
      header.data.extent.width = vp_width;
      header.data.extent.height = vp_height;
      header.data.format = rnd::driver::texture_header::TYPE::RGBA8;
      header.usage = rnd::driver::TEXTURE_USAGE::COLOR_TARGET;
      header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
      header.mag = rnd::driver::texture_header::FILTERING::NEAREST;
      header.min = rnd::driver::texture_header::FILTERING::NEAREST;
      color_rt = txm_manager.generate_texture(target_tag, header);
    }

    rnd::driver::render_state state;
    state.depth.enabled = true;
    state.depth.test_func = rnd::driver::depth_state::func::LEQUAL;
    state.depth.write_mask = false;
    drv.set_render_state(state);

    drv.push_frame_buffer();
    drv.set_render_target(color_rt, txm_manager.find(z_pass_tag));
    drv.clear(rnd::driver::CLEAR_FLAGS::COLOR_BUFFER, {glm::vec4(0)});

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);
    common_matrix.time = (float)Timer::now();

    rnd::get_system().get_shader_manager().update_global_uniform(common_matrix);
    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    //std::sort(packet.opaque_draws.begin(), packet.opaque_draws.end(),
    //          [](const rnd::draw_call_t &a, const rnd::draw_call_t &b) {
    //            return a.sort_key < b.sort_key;
    //          });

    for (const auto &dc : packet.opaque_draws) {
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
