#pragma once
#include <snk/editor/edt_problem_list.h>
#include <vector>

namespace scn
{
	class context_set;
}

namespace edt
{
	class scene_document;

	// What is wrong with the systems of the level's worlds (a name that is not registered, the same system twice, a phase
	// that was never added, an ordering that cannot hold), read from the plans of the contexts, as the problems of the
	// source "world". Without it such a system is only skipped, with a line in the log (#549). Main thread only.
	class world_problem_publisher
	{
	public:
		static constexpr const char* source = "world";

		// Sets the source when the problems differ from the last call; clears it when there are none. `level` null: none.
		void refresh(problem_list& problems, const scene_document& scene, const scn::context_set* level);

		// The problems alone, for the tests.
		static std::vector<problem> collect(const scene_document& scene, const scn::context_set* level);

	private:
		std::vector<problem> m_last;
		bool m_published = false;
	};
}
