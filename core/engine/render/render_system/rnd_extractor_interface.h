#pragma once
#include "rnd_frame_context.h"

namespace rnd {
	struct extractor_interface {
		virtual ~extractor_interface() = default;
		virtual void extract(frame_context& context) = 0;
	};
}