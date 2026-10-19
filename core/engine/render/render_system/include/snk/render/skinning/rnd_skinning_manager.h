#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rnd
{
	// The vertex weights of every skin. The GPU side (index buffer, bone matrices) is rnd::per_draw_data's.
	class skinning_manager
	{
	public:
		skinning_manager() = default;
		~skinning_manager() = default;
		skinning_manager(const skinning_manager&) = delete;
		skinning_manager& operator=(const skinning_manager&) = delete;
		skinning_manager(skinning_manager&&) = delete;
		skinning_manager& operator=(skinning_manager&&) = delete;

		void register_weights(res::tag tag, std::vector<std::vector<uint32_t>> weights) {
			m_weights_registry[tag] = std::move(weights);
		}

		// The contents of the skin's IndexesBuffer (skinning_lib.glsl): column count, column offsets, then the
		// bone indices and weights of every column. Empty for a skin without registered weights.
		std::vector<uint32_t> build_indices(const res::tag& skin) const;

	private:
		std::unordered_map<res::tag, std::vector<std::vector<uint32_t>>> m_weights_registry;
	};
}
