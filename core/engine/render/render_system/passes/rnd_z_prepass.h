#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_pass_interface.h>
#include <snk/resource/res_tag.h>

namespace rnd {
class SNK_API z_prepass : public render_pass_interface {
public:
  // The engine passes res::resource_system::engine_file tags: a project cannot replace them (#929).
  z_prepass(res::tag vertex_shader, res::tag fragment_shader);
  virtual void execute(render_context &context,
                       driver::driver_interface &drv) override;

private:
  res::tag m_vertex_shader;
  res::tag m_fragment_shader;
};
} // namespace rnd
