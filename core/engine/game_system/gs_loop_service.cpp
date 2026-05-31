#include "gs_loop_service.h"
#include "level/scn_level_manager.h"
#include "rnd_frame_assembler.h"
#include "rnd_render_system.h"
#include "wnd_window_system.h"
#include "eng_profiler.h"


gs::gs_loop_service::gs_loop_service() : stop_requested(false) {}

gs::gs_loop_service::~gs_loop_service() {}

void gs::gs_loop_service::init(ds::app_data_storage &storage) {
  auto &window_system_ref = storage.require<wnd::window_system>();

  window_system_ref.init_windows_frame_time();
  previous_time = std::chrono::steady_clock::now();

  register_core_frame_stages(m_stages);
}

void gs::gs_loop_service::on_step(ds::app_data_storage &storage) {
  PROFILE_FRAME("GameThread");
  auto current_time = std::chrono::steady_clock::now();
  std::chrono::duration<float> duration = current_time - previous_time;
  previous_time = current_time;

  rnd::frame_context context;
  frame_step_context ctx{ storage, duration, context };
  m_stages.run(ctx);

  stop_requested = storage.require<wnd::window_system>().is_stop_running();
}

bool gs::gs_loop_service::should_stop() const { return stop_requested; }