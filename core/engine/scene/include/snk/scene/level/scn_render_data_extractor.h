#pragma once
#include <snk/render/rnd_extractor_interface.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/resource/res_tag.h>
#include <memory>
#include <unordered_set>
#include <vector>

namespace scn {
	struct render_data_extractor : public rnd::extractor_interface {
		render_data_extractor(context_manager& contexts)
			: m_contexts(contexts)
		{}
		virtual void extract(rnd::frame_context& context) override;
	private:
		context_manager& m_contexts;
		// Geometries already named in the warning of a mesh range no draw call can say: one line each, not one a frame.
		std::unordered_set<res::tag> m_warned_ranges;
	};
}