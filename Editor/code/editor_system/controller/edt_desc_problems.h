#pragma once
#include <snk/editor/edt_problem_list.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_tag.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What went wrong in the descs the editor loaded, as the problems of the source "desc" (#596): one set per scope
	// ("level", "drop", "prefab:<tag>"), so a clean load of one does not hide the errors of another. The source holds the
	// union of the scopes and goes away when every scope is empty. Main thread only.
	class desc_problem_publisher
	{
	public:
		static constexpr const char* source = "desc";

		// The scope's problems become these diagnostics (none: the scope is dropped). A diagnostic of `copy` (the
		// editor's working copy of a level) is shown as one of `file`.
		void report(problem_list& problems, const std::string& scope, const std::vector<res::diagnostic>& items,
			const res::tag& copy = {}, const res::tag& file = {});
		void clear(problem_list& problems, std::string_view scope);

		// One row: severity, the resource (res:// relative, any other tag whole), the kind as the code, the reason, and the
		// JSON pointer as the first note, as the source "world" keeps it.
		static problem to_problem(const res::diagnostic& item);

	private:
		struct scope_problems
		{
			std::vector<problem> items;
			// The "N more diagnostics" of the scope's collector: all scopes together show as one row.
			std::size_t dropped = 0;
		};

		void publish(problem_list& problems);

		std::map<std::string, scope_problems, std::less<>> m_scopes;
		bool m_published = false;
	};
}
