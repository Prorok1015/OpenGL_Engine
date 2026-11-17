#include <snk/desc/desc_guid.h>
#include <snk/resource/res_asset_id.h>

std::string desc::generate_guid()
{
	return res::to_text(res::generate_asset_id());
}
