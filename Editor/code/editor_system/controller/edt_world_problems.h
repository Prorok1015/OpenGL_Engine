#pragma once
#include <snk/editor/edt_problem_list.h>
#include <optional>
#include <string>
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

		// The world a row of the source is about (Problems' "Go to"); nullopt for a row the source does not hold now.
		std::optional<std::string> world_of(const problem& row) const;

	private:
		struct world_problem
		{
			problem item;
			std::string world;
		};
		static std::vector<world_problem> collect_by_world(const scene_document& scene, const scn::context_set* level);

		std::vector<problem> m_last;
		// Parallel to m_last.
		std::vector<std::string> m_worlds;
		bool m_published = false;
	};
}
