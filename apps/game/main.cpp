#include <snk/game_host/game_host.h>

// The engine's own game executable (#591): no game module compiled in, the project's start level only.
int main(int argc, char* argv[])
{
	return snk::game_host::run(argc, argv, {});
}
