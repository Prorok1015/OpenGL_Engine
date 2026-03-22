#include "scn_skin_weights_desc.h"
#include "rnd_render_system.h"
#include "logger/engine_log.h"
#include "skinning/rnd_skinning_manager.h"

void scn::skin_weights_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("tag"))
		registration_tag = json::value_to<res::tag>(*p);

	if (auto const* p = data.if_contains("weights")) {
		auto const& arr = p->as_array();
		weights.clear();
		weights.reserve(arr.size());
		for (auto const& elem : arr) {
			auto const& vtx_arr = elem.as_array();
			std::vector<uint32_t> column;
			column.reserve(vtx_arr.size());
			for (auto const& v : vtx_arr) {
				column.push_back(static_cast<uint32_t>(v.to_number<uint64_t>()));
			}
			weights.push_back(std::move(column));
		}
	}
}

void scn::skin_weights_desc::serialize(json::object& data) const
{
	if (registration_tag.is_valid())
		data["tag"] = json::value_from(registration_tag);

	json::array arr;
	arr.reserve(weights.size());
	for (auto const& column : weights) {
		json::array vtx_arr;
		vtx_arr.reserve(column.size());
		for (uint32_t v : column) {
			vtx_arr.push_back(static_cast<uint64_t>(v));
		}
		arr.push_back(std::move(vtx_arr));
	}
	data["weights"] = std::move(arr);
}

void scn::assemble_skin_weights(entt::registry& reg, entt::entity e, const skin_weights_desc& desc, std::string_view name)
{
	if (desc.weights.empty() || !desc.registration_tag.is_valid()) {
		egLOG_WARN("assembler", "skin_weights_desc: empty weights or invalid tag");
		return;
	}

	if (auto* skm = rnd::get_system().get_skinning_manager()) {
		skm->register_weights(desc.registration_tag, desc.weights);
	}
}
