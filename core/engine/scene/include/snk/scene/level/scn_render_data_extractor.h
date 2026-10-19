#pragma once
#include <snk/render/rnd_extractor_interface.h>
#include <snk/scene/level/scn_level_manager.h>
#include <memory>
#include <vector>

namespace rnd { struct compiled_material; }

namespace scn {
	struct render_data_extractor : public rnd::extractor_interface {
		render_data_extractor(level_manager& level_manager)
			: m_level_manager(level_manager)
		{}
		virtual void extract(rnd::frame_context& context) override;
	private:
		level_manager& m_level_manager;
		// Materials replaced during this extraction: draws already pushed may still point at them.
		std::vector<std::shared_ptr<rnd::compiled_material>> m_retired_materials;
	};
}