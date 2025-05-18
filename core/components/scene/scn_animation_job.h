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
		void load_bone_offsets(ecs::entity ent, std::vector<glm::mat4>& out, entt::registry& registry);
		void update_animation_system(scn::delta_time dt, entt::registry& registry);
		void calc_world_transforms(entt::registry& registry, ecs::entity ent, const float ticks, const res::animation& anim, std::vector<glm::mat4>& out);
		void update_nodes_animation_system(entt::registry& registry, const scn::delta_time& dt);
	};
}