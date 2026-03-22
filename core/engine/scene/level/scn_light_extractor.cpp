#include "scn_light_extractor.h"
#include "rnd_light_data.hpp"
#include "scn_model.h"
#include "scn_world.h"

namespace scn {
	void light_extractor::extract(rnd::frame_context& context) {
		auto& level = m_level_manager.get_level();
		rnd::scene_lights_t scene_lights;
		
		level.for_each_world([&](scn::world& w) {
			auto& reg = w.state();

			for (auto [l_ent, light] : reg.view<scn::directional_light>().each()) {
				rnd::scene_lights_t::directional_light_t dl;
				dl.direction = light.direction;
				dl.diffuse   = light.diffuse;
				dl.ambient   = light.ambient;
				dl.specular  = light.specular;
				scene_lights.directional.push_back(dl);
			}
		});
		
		context.data.construct<rnd::scene_lights_t>(std::move(scene_lights));
	}
}
