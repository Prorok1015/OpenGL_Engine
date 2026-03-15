#pragma once
#include "rnd_render_pass_interface.h"

namespace rnd {
class composition_pass : public render_pass_interface {
public:
  composition_pass();
  virtual ~composition_pass() = default;
  virtual void execute(frame_context &context,
                       driver::driver_interface &drv) override;

private:
  std::unique_ptr<driver::vertex_array_interface> m_va;
  std::shared_ptr<driver::buffer_interface> m_vbo;
  std::shared_ptr<driver::buffer_interface> m_ibo;
};
} // namespace rnd
