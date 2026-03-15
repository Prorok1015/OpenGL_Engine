#pragma once
#include "rnd_extractor_interface.h"
#include "scn_level_manager.h"

namespace scn {
	struct light_extractor : public rnd::extractor_interface {
		explicit light_extractor(level_manager& lm) : m_level_manager(lm) {}
		virtual void extract(rnd::frame_context& context) override;
	private:
		level_manager& m_level_manager;
	};
}
