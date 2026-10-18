#pragma once
#include <string>
#include <snk/common/snk_api.h>

namespace desc
{
	// UUID v4 as a lowercase string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx, y in {8,9,a,b}.
	SNK_API std::string generate_guid();
}
