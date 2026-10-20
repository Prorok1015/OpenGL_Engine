#pragma once
#include <boost/describe/enum.hpp>

namespace rnd::driver
{
	// How the indices of a draw assemble into primitives. The one list of topologies: the material description
	// names its render_mode with these enumerators, so a value the driver cannot draw cannot be written.
	enum class RENDER_MODE
	{
		TRIANGLE,
		TRIANGLE_STRIP,
		LINE,
		LINE_STRIP,
		POINT,
	};

	BOOST_DESCRIBE_ENUM(RENDER_MODE, TRIANGLE, TRIANGLE_STRIP, LINE, LINE_STRIP, POINT)
}
