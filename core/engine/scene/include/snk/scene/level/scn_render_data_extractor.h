#pragma once
#include <snk/render/rnd_extractor_interface.h>
#include <snk/scene/level/scn_context_manager.h>
#include <memory>
#include <vector>

namespace scn {
	struct render_data_extractor : public rnd::extractor_interface {
		render_data_extractor(context_manager& contexts)
			: m_contexts(contexts)
		{}
		virtual void extract(rnd::frame_context& context) override;
	private:
		context_manager& m_contexts;
	};
}