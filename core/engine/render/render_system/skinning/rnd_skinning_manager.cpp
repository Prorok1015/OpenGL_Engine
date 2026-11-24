#include <snk/render/skinning/rnd_skinning_manager.h>
#include <vector>

std::vector<uint32_t> rnd::skinning_manager::build_indices(const res::tag& skin) const
{
     ++m_index_builds;
     auto it = m_weights_registry.find(skin);
     if (it == m_weights_registry.end()) return {};
     const auto& columns = it->second.columns;

     uint32_t numColumns = static_cast<uint32_t>(columns.size());

     std::vector<uint32_t> offsets(numColumns + 1, 0);
     uint32_t currentOffset = 0;
     for (uint32_t j = 0; j < numColumns; ++j) {
         offsets[j] = currentOffset;
         currentOffset += static_cast<uint32_t>(columns[j].size());
     }
     offsets[numColumns] = currentOffset;

     std::vector<uint32_t> bufferData;
     bufferData.reserve(1 + (numColumns + 1) + currentOffset);

     bufferData.push_back(numColumns);

     bufferData.insert(bufferData.end(), offsets.begin(), offsets.end());

     for (const auto& col : columns) {
         bufferData.insert(bufferData.end(), col.begin(), col.end());
     }

     return bufferData;
}

rnd::skinning_manager::skinning_manager() = default;

rnd::skinning_manager::~skinning_manager() = default;

void rnd::skinning_manager::register_weights(res::tag tag, std::vector<std::vector<uint32_t>> weights)
{
	auto& entry = m_weights_registry[tag];
	entry.columns = std::move(weights);
	entry.version = ++m_generation;
}

std::uint64_t rnd::skinning_manager::generation() const noexcept
{
	return m_generation;
}

std::uint64_t rnd::skinning_manager::version(const res::tag& skin) const
{
	const auto it = m_weights_registry.find(skin);
	return it == m_weights_registry.end() ? 0 : it->second.version;
}

std::size_t rnd::skinning_manager::index_builds() const noexcept
{
	return m_index_builds;
}
