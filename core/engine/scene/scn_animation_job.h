#include "ecs_system.h"
#include "ecs_entity.h"
#include "res_mesh.hpp"
#include "scn_model.h"

namespace scn
{
	class animation_job : public ecs::job_base
	{
	public:
		animation_job() = default;

		// Inherited via job_base
		void init(entt::organizer& organizer, entt::registry& registry) override;
		void deinit(entt::organizer& organizer, entt::registry& registry) override;

	private:
		void update_bone_offsets_system(entt::registry& registry);
		void update_nodes_animation_system(entt::registry& registry, const scn::delta_time& dt);
	};
}