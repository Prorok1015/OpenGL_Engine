#pragma once
#include "edt_desc_location.h"
#include <snk/editor/edt_problem_list.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ecs { class system_registry; }
namespace res { class resource_system; }
namespace scn { class ecs_assembler; }
namespace desc { class desc_system; }

namespace edt
{
	class scene_document;

	// One finding of the check before Play (#597): the row Problems shows and where its "Go to" leads.
	struct preflight_problem
	{
		problem item;
		desc_target target;
	};

	// A component of a node of a world, as the check walks them.
	struct preflight_component
	{
		std::string_view world;
		// Node ids from the world's root; empty: the root.
		const std::vector<std::uint64_t>& path;
		std::string_view key;
		const scn::prefab_desc::prefab_comp_node& node;
		// JSON pointer of the component in its world ("/children/2/components/mesh").
		const std::string& json_path;
	};

	// What the check reads; a null service skips the checks that need it.
	struct play_preflight_services
	{
		const ecs::system_registry* systems = nullptr;
		const desc::desc_system* descs = nullptr;
		const res::resource_system* resources = nullptr;
		const scn::ecs_assembler* assembler = nullptr;
		// More checks of one component (the schemas of #606 once they land); it appends what it finds. Null: none.
		std::function<void(const preflight_component&, std::vector<preflight_problem>&)> check_component{};
	};

	// Checks the document before Play: the systems of every world (a name not registered, twice, a phase never added,
	// an ordering that cannot hold), the resources its components refer to (in a field its schema types as a tag: a
	// reference res::tag or the rule of #697 refuses, a file that is not there; a __parent that did not load: errors; in
	// any other field only a reference the level may make to a file that is not there, a warning) and its components (a type not registered or
	// whose fields its schema refuses are errors, a type without a spawner a warning). Reads only: nothing is loaded or
	// written (a file is only looked for). Errors stop Play until a person confirms; warnings only show.
	[[nodiscard]] std::vector<preflight_problem> run_play_preflight(const scene_document& scene, const play_preflight_services& services);

	[[nodiscard]] bool has_errors(const std::vector<preflight_problem>& problems);
	[[nodiscard]] std::size_t error_count(const std::vector<preflight_problem>& problems);
	// {problems: [{severity, code, message, world, path? (hex node ids), json_path?}]}: the answer of play.start and
	// editor.mode.toggle when the check refuses Play.
	[[nodiscard]] boost::json::object problems_json(const std::vector<preflight_problem>& problems);

	// The source "play" of Problems: the last check before Play, with where each row leads. Main thread only.
	class play_preflight_publisher
	{
	public:
		static constexpr const char* source = "play";

		// The source becomes these rows; none clears it.
		void publish(problem_list& problems, std::vector<preflight_problem> found);
		std::optional<desc_target> target_of(const problem& row) const;

	private:
		std::vector<preflight_problem> m_last;
	};
}
