#include "edt_editor_init.h"
#include "edt_editor_system.h"
#include "desc_system.h"

namespace components {

	void editor_init(ds::app_data_storage& data)
	{
		auto& desc_system = data.require<desc::desc_system>();
		auto& editor = data.construct<edt::editor_system>(desc_system);

		desc_system.register_desc2<edt::editor_test_parent_desc>("editor_parent");
		desc_system.register_desc2<edt::editor_test_desc>("editor_test");
		desc_system.register_desc2<edt::editor_test_field_desc>("editor_field_desc");
		desc_system.register_desc2<edt::editor_test_sub_field_desc>("editor_test_sub");
	}

	void editor_term(ds::app_data_storage& data)
	{

		auto& desc_system = data.require<desc::desc_system>();
		data.destruct<edt::editor_system>();
	}

}