#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

// The parsed "migrations" of a runtime schema. Internal to desc_system: what crosses into a module is the JSON array,
// so no new type enters the module layout key.
namespace desc::detail
{
	struct schema_migration
	{
		int from = 0;
		std::vector<std::pair<std::string, std::string>> renames;
		std::vector<std::string> removals;
		std::vector<std::pair<std::string, boost::json::value>> defaults;
	};

	// Throws std::runtime_error naming what is wrong (the text of schema_migrations_problem).
	std::vector<schema_migration> parse_schema_migrations(const boost::json::array& migrations, int version);

	// "rename /hp -> /health; remove /legacy; default /armor": the line of the migration plan and report.
	std::string summary_of(const schema_migration& step);

	void apply(const schema_migration& step, boost::json::object& object);
}
