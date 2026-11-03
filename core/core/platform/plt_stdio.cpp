#include <snk/platform/plt_stdio.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdint>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _WIN32

int platform::redirect_stdout(int stdout_fd, int stderr_fd)
{
	const int saved = _dup(stdout_fd);
	if (saved < 0) {
		return -1;
	}
	// Programs the editor starts must not hold the client's pipe open.
	SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(saved)), HANDLE_FLAG_INHERIT, 0);
	if (_dup2(stderr_fd, stdout_fd) != 0) {
		_close(saved);
		return -1;
	}
	return saved;
}

int platform::take_stdout_for_protocol()
{
	std::fflush(stdout);
	const HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
	const int out = _fileno(stdout);
	const int err = _fileno(stderr);
	int saved = -1;
	if (out < 0 || err < 0) {
		// No C runtime stream (a process without a console): only the handle is there.
		const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
		if (output == nullptr || output == INVALID_HANDLE_VALUE) {
			return -1;
		}
		saved = _open_osfhandle(reinterpret_cast<std::intptr_t>(output), _O_WRONLY | _O_BINARY);
		if (saved < 0) {
			return -1;
		}
		SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
	} else {
		saved = redirect_stdout(out, err);
		if (saved < 0) {
			return -1;
		}
		_setmode(saved, _O_BINARY);
	}
	SetStdHandle(STD_OUTPUT_HANDLE, error_handle);
	return saved;
}

bool platform::write_all(int fd, std::string_view data)
{
	const char* next = data.data();
	std::size_t left = data.size();
	while (left > 0) {
		const unsigned chunk = static_cast<unsigned>(std::min<std::size_t>(left, std::size_t{ 1 } << 30));
		const int written = _write(fd, next, chunk);
		if (written <= 0) {
			return false;
		}
		next += written;
		left -= static_cast<std::size_t>(written);
	}
	return true;
}

#else

int platform::redirect_stdout(int stdout_fd, int stderr_fd)
{
	const int saved = ::fcntl(stdout_fd, F_DUPFD_CLOEXEC, 0);
	if (saved < 0) {
		return -1;
	}
	while (::dup2(stderr_fd, stdout_fd) < 0) {
		if (errno != EINTR) {
			::close(saved);
			return -1;
		}
	}
	return saved;
}

int platform::take_stdout_for_protocol()
{
	std::fflush(stdout);
	const int saved = redirect_stdout(::fileno(stdout), ::fileno(stderr));
	if (saved >= 0) {
		std::signal(SIGPIPE, SIG_IGN);
	}
	return saved;
}

bool platform::write_all(int fd, std::string_view data)
{
	const char* next = data.data();
	std::size_t left = data.size();
	while (left > 0) {
		const ssize_t written = ::write(fd, next, left);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		next += written;
		left -= static_cast<std::size_t>(written);
	}
	return true;
}

#endif
