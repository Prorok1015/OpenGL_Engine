#pragma once
#include "scn_prefab_desc.h"

namespace scn
{
	class world_desc : public scn::prefab_desc
	{
	public:
		static constexpr std::string_view __type = "world_desc";

		using base_type = scn::prefab_desc;
		world_desc() = default;
		~world_desc() = default;
		world_desc(const world_desc&) = default;
		world_desc(world_desc&&) = default;
		world_desc& operator=(const world_desc&) = default;
		world_desc& operator=(world_desc&&) = default;
		virtual void copy_to(desc_base& other) const override
		{
			world_desc& other_desc = static_cast<world_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		const std::vector<std::string>& get_systems() const { return systems; }
		std::string_view get_name() const { return name; }
	private:
		std::string name;
		std::vector<std::string> systems;
	};
}
