// #558: desc code in a game module built against the installed SDK, linked as the project template links `game`.
// Copying a prefab_node instantiates boost::json::object's copy in this library, which links only when the module
// has Boost.JSON (MSVC: snk::thirdparty_boost_json; elsewhere the engine's copy).
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>

#if defined(_WIN32)
#define SNK_CONSUMER_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define SNK_CONSUMER_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

SNK_CONSUMER_MODULE_EXPORT scn::prefab_desc::prefab_node* snk_consumer_copy_prefab_node(const scn::prefab_desc::prefab_node* node)
{
	auto* copy = new scn::prefab_desc::prefab_node(*node);
	copy->components["module"].overrides["written_in_module"] = boost::json::array{ 1, 2, 3 };
	return copy;
}
