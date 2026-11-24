// #1304: a library that travels beside a game module (test_game_dll_sibling needs it): the host must load it from
// the module's held copy folder, never from where it was built.
#if defined(_WIN32)
#define SNK_TEST_SIBLING_EXPORT __declspec(dllexport)
#else
#define SNK_TEST_SIBLING_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SNK_TEST_SIBLING_EXPORT int snk_test_sibling_value();
extern "C" SNK_TEST_SIBLING_EXPORT int snk_test_sibling_value()
{
	return 7;
}
