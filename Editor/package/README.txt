Snake - OpenGL Engine editor and SDK

Requirements: Windows 10/11 x64, a GPU with OpenGL 4.5+ drivers. The Microsoft C++ runtime
is included (bin/), nothing else needs to be installed.

Run from this folder:
    bin\editor.exe --project projects\sandbox

The editor reads config\editor.cfg next to bin\ by itself, from any working directory; pass
--config <file> to use another configuration.

Keep the folders side by side: the config refers to the engine's base resources as
../res_core/ and to the editor's own icons and templates as ../data/, relative to the
config file. projects\sandbox is the test project; open another one with --project <folder>.
Logs are written to logs\ relative to the working directory.

The SDK (include/, lib/, lib/cmake/snake_sdk) builds game modules: the editor finds it next to
itself. find_package(snake_sdk) gives snk::engine (snk_engine.dll, the editor and game.dll) and
snk::engine_static (one executable, Shipping). The Debug engine is a separate archive
(Snake-<version>-windows-x64-debug.zip): unpack it over this folder.

Compiler: Visual Studio 2022 17.14 or newer (MSVC toolset 14.44, _MSC_VER 1944, or newer). The
engine is built with MSVC 14.44; a game module must be built with the same MSVC or a newer one and
with the DLL runtime (/MD). find_package(snake_sdk) refuses an older MSVC or a static runtime, and
the editor refuses to load a module built with an older MSVC than the engine's. The project
template's DebugGame configuration (/Od, /MD) links the Release engine; a Debug (/MDd) build
needs the Debug archive above.
