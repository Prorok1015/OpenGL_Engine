#include "rnd_composition_pass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_context.h>
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/driver/rnd_buffer_layout.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include "rnd_pass_pipelines.h"
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
struct screen_vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 uv;
};

composition_pass::composition_pass() {}

void composition_pass::execute(render_context &context,
                               driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Composition");
  PROFILE_GPU_SCOPE("Pass.Composition");
  const auto packets = context.packets();
  if (packets.empty()) {
    return;
  }

  // Lazy initialization of OpenGL resources
  if (!m_va) {
    m_va = drv.create_vertex_array();

    m_vbo = drv.create_buffer();
    m_vbo->reserve(4 * sizeof(screen_vertex));
    m_vbo->set_layout(
        {{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
         {rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
         {rnd::driver::SHADER_DATA_TYPE::VEC2_F, "texture_position"}});
    m_va->add_vertex_buffer(m_vbo);

    m_ibo = drv.create_buffer();
    m_va->set_index_buffer(m_ibo);
  }

  for (const auto& packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    const auto target = rnd::select_camera_target(packet.camera);
    const bool offscreen = target.kind == rnd::render_target_kind::texture;
    res::tag tp_accum_tag = rnd::resolve_tp_accum_target(packet.camera);
    res::tag tp_reveal_tag = rnd::resolve_tp_reveal_target(packet.camera);
    auto color_rt = offscreen ? context.find_texture(target.texture) : nullptr;
    auto color_tp_rt = context.find_texture(tp_accum_tag);
    auto waight_tp_rt = context.find_texture(tp_reveal_tag);

    if ((offscreen && !color_rt) || !color_tp_rt || !waight_tp_rt)
      continue;

    rnd::driver::render_pass_desc pass;
    pass.debug_name = "composition";
    if (offscreen) {
      pass.colors.push_back({.texture = rnd::driver::handle_of(color_rt)});
    } else {
      pass.backbuffer = true;
    }
    drv.begin_render_pass(pass);

    rnd::driver::pipeline_desc state;
    state.depth.enabled = true;
    state.depth.test_func = rnd::driver::depth_state::func::ALWAYS;
    state.depth.write_mask = false;
    rnd::driver::blend_state blend;
    blend.enabled = true;
    blend.src_factor = rnd::driver::blend_state::factor::SRC_ALPHA;
    blend.dst_factor = rnd::driver::blend_state::factor::ONE_MINUS_SRC_ALPHA;
    state.blend = {blend};
    rnd::driver::set_target_formats(drv, state, pass);
    state.set_layouts = context.layouts().composite();
    rnd::pass_pipelines pipelines(drv, context, state);

    drv.set_viewport(glm::ivec4{packet.camera.viewport.x,
                                packet.camera.viewport.y, vp_width, vp_height});

    screen_vertex screen[4];
    screen[0].position = glm::vec3{-1.f, -1.f, 0.f};
    screen[1].position = glm::vec3{-1.f, 1.f, 0.f};
    screen[2].position = glm::vec3{1.f, 1.f, 0.f};
    screen[3].position = glm::vec3{1.f, -1.f, 0.f};

    m_vbo->set_data_ptr(screen, 4);

    unsigned int indeces[]{0, 1, 2, 0, 3, 2};
    m_ibo->set_data_ptr(indeces, std::size(indeces));

    rnd::shader_config desc;
    desc.cdata.program =
        rnd::shader_config::shader_program_data::build()
            .set_vertex_shader(res::tag::make("shaders/composition.vert"))
            .set_fragment_shader(res::tag::make("shaders/composition.frag"));

    const auto variant = context.variants().intern(desc.cdata);
    if (pipelines.bind(variant)) {
      rnd::driver::bind_group_desc inputs;
      inputs.layout = context.layouts().composition;
      inputs.entries.push_back({rnd::driver::binding_type::texture, rnd::bindings::accum_slot, {}, 0, 0, rnd::driver::handle_of(color_tp_rt)});
      inputs.entries.push_back({rnd::driver::binding_type::texture, rnd::bindings::reveal_slot, {}, 0, 0, rnd::driver::handle_of(waight_tp_rt)});
      drv.set_bind_group(rnd::driver::pass_set, drv.create_bind_group(inputs, rnd::driver::lifetime::transient));
      drv.draw_indices(m_va, std::size(indeces));
    }

    drv.end_render_pass();
  }
}
} // namespace rnd
