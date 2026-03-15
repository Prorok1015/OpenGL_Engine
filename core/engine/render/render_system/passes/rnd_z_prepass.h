#pragma once
#include "rnd_render_pass_interface.h"

namespace rnd {
class z_prepass : public render_pass_interface {
public:
  virtual void execute(frame_context &context,
                       driver::driver_interface &drv) override;
};
} // namespace rnd
