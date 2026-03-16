#pragma once
#include "rnd_frame_context.h"
#include "rnd_extractor_interface.h"

namespace rnd {
	struct frame_assembler {
		void add_extractor(std::shared_ptr<extractor_interface> extractor) {
			extractors.push_back(extractor);
		}

		void remove_extractor(std::shared_ptr<extractor_interface> extractor) {
			extractors.erase(std::remove(extractors.begin(), extractors.end(), extractor));
		}

		void build_frame(frame_context& context) {
			for (auto& extractor : extractors) {
				extractor->extract(context);
			}
		}
	private:
		std::vector<std::shared_ptr<extractor_interface>> extractors;
	};
}