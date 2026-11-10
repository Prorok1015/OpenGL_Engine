#pragma once
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace res { class resource_system; }

namespace edt
{
	// #681: a model file is parsed in the editor's process (the model importer adapter), so an agent may bring one in
	// only through file.import_model, which a person approves. Every other command an agent calls is refused when a
	// string of its arguments names a model file, wherever it would end up (a component field, a desc written now and
	// loaded later). Models the project's own files refer to are the project's: they load as for a person.

	// The extension the resolver would pick an adapter by, compared without case: `\` as `/`, trailing spaces and dots
	// dropped (a Windows file name ignores them), then what follows the last '.' of the last segment. Empty: none.
	std::string model_check_extension(std::string_view text);

	struct named_model
	{
		std::string pointer; // JSON pointer of the string in the arguments
		std::string value;
		std::string extension;
	};

	// The first string of `args` (values at any depth, not keys) whose extension is one of `model_extensions`.
	std::optional<named_model> find_named_model(const boost::json::value& args, std::span<const std::string> model_extensions);

	// The check for command_registry::set_agent_argument_check: the extensions are those the model importer serves
	// in `resources` at the time of the call (none in a build without it).
	cmd::command_registry::agent_argument_check make_agent_model_check(const res::resource_system& resources);
}
