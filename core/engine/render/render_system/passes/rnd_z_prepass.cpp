#include "rnd_z_prepass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/render/rnd_pass_pipelines.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/driver/rnd_ssbo_buffer_interface.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
void z_prepass::execute(frame_context &context, driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.ZPrepass");
  PROFILE_GPU_SCOPE("Pass.ZPrepass");
  if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
    return;
  }

  auto &packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

  auto &txm_manager = *context.textures;
  auto &geom_manager = *context.geometry;
  rnd::global_params common_matrix;

  for (const auto &packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    res::tag z_pass_tag = rnd::resolve_depth_target(packet.camera);

    auto z_pass_rt = rnd::render_targets::ensure(
        txm_manager, z_pass_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::D32,
        rnd::driver::texture_usage::depth_target,
        {.min = rnd::driver::texture_header::FILTERING::NEAREST,
         .mag = rnd::driver::texture_header::FILTERING::NEAREST,
         .wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_BORDER});

    rnd::driver::render_pass_desc pass;
    pass.depth_stencil = rnd::driver::attachment_desc{
        .texture = z_pass_rt, .load = rnd::driver::load_op::clear};
    pass.debug_name = "z_prepass";
    drv.begin_render_pass(pass);

    rnd::driver::pipeline_desc state;
    state.depth = {.enabled = true, .write_mask = true, .test_func = rnd::driver::depth_state::func::LESS};
    rnd::driver::set_target_formats(state, pass);
    rnd::pass_pipelines pipelines(drv, *context.shaders, state);

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);

    context.shaders->update_global_uniform(common_matrix);

    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

	auto z_prepass_program = rnd::shader_config::shader_program_data::build()
        .set_vertex_shader(res::tag::make("shaders/z_prepass.vert"))
        .set_fragment_shader(res::tag::make("shaders/z_prepass.frag"));

    auto& variants = context.shaders->variants();
    std::pmr::vector<rnd::variant_id> z_variants(ds::frame_allocator());
    z_variants.resize(variants.size());

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

      rnd::variant_id& variant = z_variants[dc.variant.value];
      if (!variant.is_valid()) {
        variant = variants.with_program(dc.variant, z_prepass_program);
      }
      if (!pipelines.bind(variant))
        continue;
      rnd::configure_pass(context, variant, *dc.material, &dc.transform);

      if (dc.bone_matrices && !dc.bone_matrices->empty() && dc.skinning_tag.is_valid()) {
          if (auto* skm = context.skinning) {
              skm->bind_skin(&drv, dc.skinning_tag, *dc.bone_matrices);
          }
      }

      drv.draw_indices(va, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    drv.end_render_pass();
  }
}
} // namespace rnd
