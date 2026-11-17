#pragma once
#include <snk/scene/level/scn_input_stack_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/common/snk_api.h>

namespace scn
{
	class SNK_API world_desc : public scn::prefab_desc
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
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		const std::vector<std::string>& get_systems() const { return systems; }
		// The names of the systems the world runs, in the order they are scheduled within a phase.
		void set_systems(std::vector<std::string> names) { systems = std::move(names); }
		std::string_view get_name() const { return name; }
		// Input stacking of the context (higher is on top, UI = 1); see gs::route_input.
		int get_input_layer() const { return input_layer; }
		// Defaults of the world's base input layer; none: the layer behaves as before the field existed.
		const std::optional<input_stack_desc>& get_input_stack() const { return input_stack; }
		void set_input_stack(std::optional<input_stack_desc> stack) { input_stack = std::move(stack); }
	private:
		std::string name;
		int input_layer = 0;
		std::vector<std::string> systems;
		// After the older fields: inline accessors of a game module built earlier keep their offsets.
		std::optional<input_stack_desc> input_stack;
	};
}
