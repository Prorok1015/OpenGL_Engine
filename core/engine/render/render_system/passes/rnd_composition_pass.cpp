#include "rnd_composition_pass.h"
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
struct screen_vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 uv;
};

composition_pass::composition_pass() {}

void composition_pass::execute(frame_context &context,
                               driver::driver_interface &drv) {
  PROFILE_SCOPE("Pass.Composition");
  PROFILE_GPU_SCOPE("Pass.Composition");
  if (!context.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
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

  auto &packets = context.data.require<std::pmr::vector<rnd::render_packet_t>>();

  auto &txm_manager = *context.textures;

  for (const auto& packet : packets) {
    int vp_width = packet.camera.viewport.z;
    int vp_height = packet.camera.viewport.w;

    if (vp_width < 1 || vp_height < 1)
      continue;

    const auto target = rnd::select_camera_target(packet.camera);
    const bool offscreen = target.kind == rnd::render_target_kind::texture;
    res::tag tp_accum_tag = rnd::resolve_tp_accum_target(packet.camera);
    res::tag tp_reveal_tag = rnd::resolve_tp_reveal_target(packet.camera);
    auto color_rt = offscreen ? txm_manager.find(target.texture) : nullptr;
    auto color_tp_rt = txm_manager.find(tp_accum_tag);
    auto waight_tp_rt = txm_manager.find(tp_reveal_tag);

    if ((offscreen && !color_rt) || !color_tp_rt || !waight_tp_rt)
      continue;

    rnd::driver::render_state state;
    state.depth.enabled = true;
    state.depth.test_func = rnd::driver::depth_state::func::ALWAYS;
    state.depth.write_mask = false;
    rnd::driver::blend_state blend;
    blend.enabled = true;
    blend.src_factor = rnd::driver::blend_state::factor::SRC_ALPHA;
    blend.dst_factor = rnd::driver::blend_state::factor::ONE_MINUS_SRC_ALPHA;
    state.blend_states = {blend};
    drv.set_render_state(state);

    rnd::driver::render_pass_desc pass;
    pass.debug_name = "composition";
    if (offscreen) {
      pass.colors.push_back({.texture = color_rt});
    } else {
      pass.backbuffer = true;
    }
    drv.begin_render_pass(pass);
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
    desc.rdata.samplers = {color_tp_rt, waight_tp_rt};

    rnd::configure_pass(context, desc);
    drv.draw_indices(m_va, rnd::RENDER_MODE::TRIANGLE, std::size(indeces));

    drv.end_render_pass();
  }
}
} // namespace rnd
