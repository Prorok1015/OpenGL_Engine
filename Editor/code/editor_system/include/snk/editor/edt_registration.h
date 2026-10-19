#pragma once
#include <snk/command/cmd_registration.h>

namespace edt
{
	// The editor's registries hand out the engine's RAII handle (cmd::registration, snk::command).
	using registration = cmd::registration;
}
