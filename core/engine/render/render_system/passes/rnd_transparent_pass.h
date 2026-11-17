#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_pass_interface.h>

namespace rnd {
class SNK_API transparent_pass : public render_pass_interface {
public:
  virtual void execute(render_context &context,
                       driver::driver_interface &drv) override;
};
} // namespace rnd
