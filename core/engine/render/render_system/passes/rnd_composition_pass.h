#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_pass_interface.h>
#include <snk/resource/res_tag.h>

namespace rnd {
class SNK_API composition_pass : public render_pass_interface {
public:
  // The engine passes res::resource_system::engine_file tags: a project cannot replace them (#929).
  composition_pass(res::tag vertex_shader, res::tag fragment_shader);
  virtual ~composition_pass() = default;
  virtual void execute(render_context &context,
                       driver::driver_interface &drv) override;

private:
  res::tag m_vertex_shader;
  res::tag m_fragment_shader;
  std::unique_ptr<driver::vertex_array_interface> m_va;
  std::shared_ptr<driver::buffer_interface> m_vbo;
  std::shared_ptr<driver::buffer_interface> m_ibo;
};
} // namespace rnd
