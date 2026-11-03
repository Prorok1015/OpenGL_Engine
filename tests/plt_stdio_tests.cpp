#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_stdio.h>
#include <cstddef>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

// platform::redirect_stdout (#452, --mcp-stdio): after it, what the process prints goes to "stderr" and only the saved
// descriptor reaches what stdout was. Pipes stand in for both; the real stdout of the test process is put back.
namespace {

#ifdef _WIN32
	bool make_pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY) == 0; }
	int dup_fd(int fd) { return _dup(fd); }
	int dup2_fd(int from, int to) { return _dup2(from, to); }
	int close_fd(int fd) { return _close(fd); }
	int read_fd(int fd, char* buffer, unsigned size) { return _read(fd, buffer, size); }
	int stdout_fd() { return _fileno(stdout); }
#else
	bool make_pipe(int fds[2]) { return ::pipe(fds) == 0; }
	int dup_fd(int fd) { return ::dup(fd); }
	int dup2_fd(int from, int to) { return ::dup2(from, to); }
	int close_fd(int fd) { return ::close(fd); }
	int read_fd(int fd, char* buffer, unsigned size) { return static_cast<int>(::read(fd, buffer, size)); }
	int stdout_fd() { return ::fileno(stdout); }
#endif

	// Everything until every write end is closed.
	std::string read_all(int fd)
	{
		std::string out;
		char buffer[4096];
		for (;;) {
			const int got = read_fd(fd, buffer, static_cast<unsigned>(sizeof(buffer)));
			if (got <= 0) {
				break;
			}
			out.append(buffer, static_cast<std::size_t>(got));
		}
		return out;
	}

}

BOOST_AUTO_TEST_SUITE(PlatformStdioTests)

BOOST_AUTO_TEST_CASE(RedirectStdout_OnlyTheSavedDescriptorReachesTheOriginalStdout)
{
	int original[2] = { -1, -1 }; // stands in for the client's pipe on stdout
	int errors[2] = { -1, -1 };   // stands in for stderr
	BOOST_REQUIRE(make_pipe(original));
	BOOST_REQUIRE(make_pipe(errors));

	std::fflush(stdout);
	const int out = stdout_fd();
	const int real_stdout = dup_fd(out);
	BOOST_REQUIRE(real_stdout >= 0);

	// No BOOST_ check while stdout is moved: its output would land in the pipes.
	const bool pointed = dup2_fd(original[1], out) >= 0;
	const int saved = pointed ? platform::redirect_stdout(out, errors[1]) : -1;
	bool written = false;
	if (saved >= 0) {
		std::printf("noise from a library\n");
		std::fflush(stdout);
		written = platform::write_all(saved, "{\"jsonrpc\":\"2.0\",\"method\":\"x\"}\n");
		close_fd(saved);
	}
	std::fflush(stdout);
	dup2_fd(real_stdout, out);
	close_fd(real_stdout);

	BOOST_REQUIRE(pointed);
	BOOST_REQUIRE(saved >= 0);
	BOOST_TEST(written);
	close_fd(original[1]);
	close_fd(errors[1]);
	BOOST_TEST(read_all(original[0]) == "{\"jsonrpc\":\"2.0\",\"method\":\"x\"}\n");
	// Windows may write the C runtime's text-mode "\r\n".
	BOOST_TEST(read_all(errors[0]).find("noise from a library") != std::string::npos);
	close_fd(original[0]);
	close_fd(errors[0]);
}

BOOST_AUTO_TEST_SUITE_END()
