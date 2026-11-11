#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>

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
