#include "rnd_z_prepass.h"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "rnd_ssbo_buffer_interface.h"
#include "skinning/rnd_skinning_manager.h"
#include "eng_profiler.h"
#include "rnd_gpu_profiler.h"

namespace rnd {
void z_prepass::execute(frame_context &context, driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.ZPrepass");
  PROFILE_GPU_SCOPE("Pass.ZPrepass");
  if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
    return;
  }

  auto &packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

  auto &txm_manager = rnd::get_system().get_texture_manager();
  auto &geom_manager = rnd::get_system().get_geom_manager();
  rnd::global_params common_matrix;

  for (const auto &packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    res::tag z_pass_tag = rnd::resolve_depth_target(packet.camera);

    auto z_pass_rt = txm_manager.find(z_pass_tag);

    if (z_pass_rt &&
        (z_pass_rt->width() != vp_width || z_pass_rt->height() != vp_height)) {
      txm_manager.remove(z_pass_tag);
      z_pass_rt = nullptr;
    }

    if (!z_pass_rt) {
      rnd::driver::texture_header header;
      header.data.extent.width = vp_width;
      header.data.extent.height = vp_height;
      header.data.format = rnd::driver::texture_header::TYPE::D32;
      header.usage = rnd::driver::TEXTURE_USAGE::DEPTH_TARGET;
      header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_BORDER;
      header.mag = rnd::driver::texture_header::FILTERING::NEAREST;
      header.min = rnd::driver::texture_header::FILTERING::NEAREST;
      z_pass_rt = txm_manager.generate_texture(z_pass_tag, header);
    }

    rnd::driver::render_state state;
    state.depth.enabled = true;
    state.depth.write_mask = true;
    state.depth.test_func = rnd::driver::depth_state::func::LESS;
    drv.set_render_state(state);

    drv.push_frame_buffer();
    drv.set_render_target(nullptr, z_pass_rt);
    drv.clear(rnd::driver::CLEAR_FLAGS::DEPTH_BUFFER);
    drv.clear(rnd::driver::CLEAR_FLAGS::COLOR_BUFFER);

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);

    rnd::get_system().get_shader_manager().update_global_uniform(common_matrix);

    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});
    /*
    rnd::shader_config shader_desc;
    shader_desc.cdata.program =
        rnd::shader_config::shader_program_data::build()
            .set_vertex_shader(res::tag::make("shaders/z_prepass.vert"))
            .set_fragment_shader(res::tag::make("shaders/z_prepass.frag"));

    rnd::configure_pass(
        shader_desc); // Note: Original material might be needed if they rely on
                      // a particular original transform format, but draw_call_t
                      // handles that?
    // Wait, the new architecture sets transform per draw call! But how?
    // In opaque pass we probably set it. But `draw_call_t` has `transform`!
    */

	auto z_prepass_program = rnd::shader_config::shader_program_data::build()
        .set_vertex_shader(res::tag::make("shaders/z_prepass.vert"))
        .set_fragment_shader(res::tag::make("shaders/z_prepass.frag"));

    for (const auto& dc : packet.opaque_draws) {
      auto *va = geom_manager.require_geometry(dc.geometry_tag);
      if (!va)
        continue;
      // Set transform per object (not supported cleanly by the old
      // update_global_uniform, usually shaders have model matrix) The new
      // architecture needs to set "model" uniform per draw call
      // shader_desc.cdata.constants["model"] = ...?
      // Wait! we need to set the material per object if we are to use their
      // push constants / uniforms. However, z_prepass just needs the transform.

      // Let's use dc.material with `z_prepass` overriden program!
      rnd::shader_config object_shader = dc.material;
      object_shader.cdata.program = z_prepass_program;
      rnd::configure_pass(object_shader);

      if (dc.bone_matrices && !dc.bone_matrices->empty() && dc.skinning_tag.is_valid()) {
          if (auto* skm = rnd::get_system().get_skinning_manager()) {
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
