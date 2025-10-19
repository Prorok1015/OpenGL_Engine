#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "desc_system.h"

namespace components {

	void editor_init(ds::AppDataStorage& data)
	{
		auto& desc_system = data.require<desc::desc_system>();
		auto& editor = data.construct<edt::editor_system>(desc_system);

		desc_system.register_desc<edt::editor_test_parent_desc>(res::tag::make("test_parent_desc.desc"));
		desc_system.register_desc<edt::editor_test_desc>(res::tag::make("test_desc.desc"));
		desc_system.register_desc<edt::editor_test_field_desc>(res::tag::make("test_field_parent_desc.desc"));
		desc_system.register_desc<edt::editor_test_field_desc>(res::tag::make("test_field_desc.desc"), "second");
		desc_system.register_desc<edt::editor_test_sub_field_desc>(res::tag::make("test_sub_field_desc.desc"), "second");
	}

	void editor_term(ds::AppDataStorage& data)
	{

		auto& desc_system = data.require<desc::desc_system>();
		desc_system.unregister_desc<edt::editor_test_desc>();
		data.destruct<edt::editor_system>();
	}

}