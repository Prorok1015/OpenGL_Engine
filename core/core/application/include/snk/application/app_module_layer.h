#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace app::modules {

	// The layer a game module registers into (MODULE_LAYER.md): a child of the root service storage that shows
	// the layer's services before the root's. Owned by the module's loader entry, destroyed after the module's
	// shutdown_services and before the module itself.
	struct module_layer
	{
		virtual ~module_layer() = default;
		virtual ds::app_data_storage& storage() = 0;
		// The module has registered and initialized: the layer goes into use (the game worlds move to it), so
		// whatever is built on it sees everything the module registered.
		virtual void activate() {}
		// Empty when the layer may go now, else why not; module_loader::unload refuses then (std::logic_error) and the
		// module stays loaded. A game module's layer refuses while one of its callbacks runs: the unload would destroy
		// the code running it (#406).
		virtual std::string unload_refusal() const { return {}; }
	};

	// Put in the root storage by the engine; module_loader asks it for the layer of a layered() module.
	struct module_layer_factory
	{
		virtual ~module_layer_factory() = default;
		virtual std::unique_ptr<module_layer> create(ds::app_data_storage& root, std::string_view module) = 0;
	};

}
