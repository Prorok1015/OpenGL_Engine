#pragma once
#include "rnd_extractor_interface.h"
#include "scn_level_manager.h"

namespace scn {
	struct render_data_extractor : public rnd::extractor_interface {
		render_data_extractor(level_manager& level_manager)
			: m_level_manager(level_manager)
		{}
		virtual void extract(rnd::frame_context& context) override;
	private:
		level_manager& m_level_manager;
	};
}