#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_context.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>

// #558: a game module that copies a prefab_node, linked as the project template links `game` (snk::engine,
// snk::engine_api and, on MSVC, a Boost.JSON of its own). The copy runs the module's Boost.JSON code.

#if defined(_WIN32)
#define SNK_TEST_COPY_EXPORT extern "C" __declspec(dllexport)
#else
#define SNK_TEST_COPY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The host owns the copy and frees it, also after this library is unloaded.
SNK_TEST_COPY_EXPORT scn::prefab_desc::prefab_node* snk_test_copy_prefab_node(const scn::prefab_desc::prefab_node* node)
{
	auto* copy = new scn::prefab_desc::prefab_node(*node);
	// Allocated here, with the default storage of the module's code.
	copy->components["mod"].overrides["written_in_module"] = boost::json::array{ 1, 2, 3 };
	return copy;
}

// #802: what module code reads of a world through world_context's accessors, which are the engine's exports since then.
SNK_TEST_COPY_EXPORT void snk_test_world_summary(const scn::world_context* world, char* out, std::size_t size)
{
	const auto& systems = world->systems();
	const std::string text = std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", world->get_id(), world->get_name(),
		static_cast<int>(world->kind()), world->desc_index(), world->input_layer(), world->extract(),
		static_cast<int>(world->mode()), world->time_scale(), systems ? static_cast<int>(systems->size()) : -1);
	const std::size_t length = std::min(text.size(), size - 1);
	std::memcpy(out, text.data(), length);
	out[length] = '\0';
}
