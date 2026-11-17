#pragma once
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <snk/common/snk_api.h>

namespace desc { class desc_system; }
namespace res { class resource_system; }

namespace scn {

	class prefab_desc;

	// The layer (format v2, `"__layer": 2`) that turns `inherited` (a parent as serialize writes it) into `target`: the nodes
	// it removes (`__remove`), adds (`__new`), the child order (`__order`), what a node changes (name, transform, a component
	// whole, `null` for a removed one, added override records). Nullopt, with the reason in `why`, when the layer cannot say
	// it (the inherited override records of a node are not a prefix of the target's, another root id).
	SNK_API std::optional<boost::json::object> make_prefab_layer(const boost::json::object& inherited,
		const boost::json::object& target, std::string& why);

	// The file form of `prefab` over its `__parent`: `body` (as serialize wrote it) becomes the header, `__parent`, `__layer`
	// and the layer, verified by reading it back the way the adapter does. Empty string on success; otherwise the reason and
	// `body` is left as it was: the caller refuses the write, nothing is silently written without the base.
	SNK_API std::string write_prefab_layer(desc::desc_system& descs, res::resource_system& resources, const prefab_desc& prefab, boost::json::object& body);
}
