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

		// Registering a skin again (its weights were reloaded) replaces the weights and counts as a change.
		void register_weights(res::tag tag, std::vector<std::vector<uint32_t>> weights) {
			auto& entry = m_weights_registry[tag];
			entry.columns = std::move(weights);
			entry.version = ++m_generation;
		}

		// Moves on whenever any skin is registered. A cache of per-skin data compares it first and looks at the
		// version of its own skins only when it differs.
		std::uint64_t generation() const noexcept { return m_generation; }

		// 0 for a skin nobody registered weights for; otherwise a value that is new after every register_weights
		// of that skin.
		std::uint64_t version(const res::tag& skin) const {
			const auto it = m_weights_registry.find(skin);
			return it == m_weights_registry.end() ? 0 : it->second.version;
		}

		// The contents of the skin's IndexesBuffer (skinning_lib.glsl): column count, column offsets, then the
		// bone indices and weights of every column. Empty for a skin without registered weights.
		std::vector<uint32_t> build_indices(const res::tag& skin) const;

		// How many times build_indices was called, for the tests of the caches that spare the work.
		std::size_t index_builds() const noexcept { return m_index_builds; }

	private:
		struct skin_weights
		{
			std::vector<std::vector<uint32_t>> columns;
			std::uint64_t version = 0;
		};

		std::unordered_map<res::tag, skin_weights> m_weights_registry;
		std::uint64_t m_generation = 0;
		mutable std::size_t m_index_builds = 0;
	};
}
