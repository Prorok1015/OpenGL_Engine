#include "rnd_z_prepass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_context.h>
#include "rnd_pass_pipelines.h"
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
void z_prepass::execute(render_context &context, driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.ZPrepass");
  PROFILE_GPU_SCOPE("Pass.ZPrepass");
  const auto packets = context.packets();
  if (packets.empty()) {
    return;
  }

  rnd::global_params common_matrix;

  for (const auto &packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    res::tag z_pass_tag = rnd::resolve_depth_target(packet.camera);

    auto z_pass_rt = context.ensure_render_target(z_pass_tag, {vp_width, vp_height},
        rnd::driver::texture_header::TYPE::D32,
        rnd::driver::texture_usage::depth_target,
        {.min = rnd::driver::texture_header::FILTERING::NEAREST,
         .mag = rnd::driver::texture_header::FILTERING::NEAREST,
         .wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_BORDER});

    rnd::driver::render_pass_desc pass;
    pass.depth_stencil = rnd::driver::attachment_desc{
        .texture = z_pass_rt,
        .load = rnd::clears_depth(packet.camera.clear) ? rnd::driver::load_op::clear : rnd::driver::load_op::load};
    pass.debug_name = "z_prepass";
    drv.begin_render_pass(pass);

    rnd::driver::pipeline_desc state;
    state.depth = {.enabled = true, .write_mask = true, .test_func = rnd::driver::depth_state::func::LESS};
    rnd::driver::set_target_formats(state, pass);
    state.set_layouts = context.layouts().scene();
    rnd::pass_pipelines pipelines(drv, context, state);

    common_matrix.view = packet.camera.view_matrix;
    common_matrix.projection = packet.camera.projection_matrix;
    common_matrix.view_position = glm::vec4(packet.camera.view_position, 1.0f);

    context.write_view(common_matrix);
    context.bind_frame();

    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

	auto z_prepass_program = rnd::shader_config::shader_program_data::build()
        .set_vertex_shader(res::tag::make("shaders/z_prepass.vert"))
        .set_fragment_shader(res::tag::make("shaders/z_prepass.frag"));

    auto& variants = context.variants();
    std::pmr::vector<rnd::variant_id> z_variants(ds::frame_allocator());
    z_variants.resize(variants.size());

    for (std::size_t i = 0; i < packet.opaque_draws.size(); ++i) {
      const auto& dc = packet.opaque_draws[i];
      auto *va = context.geometry(packet.geometry_of(dc));
      if (!va)
        continue;

      // The variant comes from the instance, not from the draw: the instance may have been compiled again after the draw was made.
      const rnd::variant_id source = context.material(dc.material).opaque;
      rnd::variant_id& variant = z_variants[source.value];
      if (!variant.is_valid()) {
        variant = variants.with_program(source, z_prepass_program);
      }
      if (!pipelines.bind(variant))
        continue;
      rnd::configure_pass(context, dc.material);
      context.bind_object(packet.opaque_object(i), packet.skin_of(dc));

      drv.draw_indices(va, dc.indices_count,
                       dc.vx_begin, dc.ind_begin);
    }

    drv.end_render_pass();
  }
}
} // namespace rnd
