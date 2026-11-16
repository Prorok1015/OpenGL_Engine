#include <snk/editor/edt_editor_extension.h>
#include <string_view>

// The smallest game_editor module: it registers nothing. Only its exported entry points are checked by unit_tests.

namespace {

class probe_editor_module final : public edt::editor_extension
{
public:
	std::string_view name() const override { return "probe_editor_module"; }

	void register_editor(edt::editor_module_context&) override {}
};

}

SNK_DECLARE_EDITOR_MODULE(probe_editor_module)
