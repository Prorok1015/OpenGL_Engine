#include "rnd_frame_upload_pass.h"
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/common/eng_profiler.h>
#include <snk/common/mem_allocator.h>
#include "rnd_gpu_profiler.h"

namespace rnd {
void frame_upload_pass::execute(render_context &context,
                                driver::driver_interface & /*drv*/) {
  PROFILE_SCOPE("Pass.FrameUpload");
  PROFILE_GPU_SCOPE("Pass.FrameUpload");
  const auto packets = context.packets();
  if (packets.empty()) {
    return;
  }

  std::size_t object_count = 0;
  std::size_t bone_count = 0;
  for (const auto &packet : packets) {
    object_count += packet.opaque_draws.size() + packet.transparent_draws.size();
    bone_count += packet.bones.matrices.size();
  }

  std::pmr::vector<rnd::object_data> objects(ds::frame_allocator());
  objects.reserve(object_count);
  std::pmr::vector<glm::mat4> bones(ds::frame_allocator());
  bones.reserve(bone_count);

  for (auto &packet : packets) {
    packet.object_base = static_cast<uint32_t>(objects.size());
    packet.bone_base = static_cast<uint32_t>(bones.size());
    bones.insert(bones.end(), packet.bones.matrices.begin(),
                 packet.bones.matrices.end());

    for (const auto &dc : packet.opaque_draws) {
      objects.push_back({dc.transform, packet.bone_base + dc.bone_offset, {}});
    }
    for (const auto &dc : packet.transparent_draws) {
      objects.push_back({dc.transform, packet.bone_base + dc.bone_offset, {}});
    }
  }

  context.upload_objects(objects, bones);
}
} // namespace rnd
