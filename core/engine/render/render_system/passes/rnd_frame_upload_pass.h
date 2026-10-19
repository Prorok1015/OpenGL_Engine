#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_pass_interface.h>

namespace rnd {
// Runs before every pass that draws: puts the per-draw data of all packets (ObjectData, bone matrices) on
// the GPU in one upload each, so a draw only binds its element.
class SNK_API frame_upload_pass : public render_pass_interface {
public:
  virtual void execute(render_context &context,
                       driver::driver_interface &drv) override;
};
} // namespace rnd
