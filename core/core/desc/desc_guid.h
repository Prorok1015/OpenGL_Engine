#pragma once
#include <string>

namespace desc
{
	// UUID v4 as a lowercase string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx, y in {8,9,a,b}.
	std::string generate_guid();
}
