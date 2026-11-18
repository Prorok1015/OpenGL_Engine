#pragma once
#include <snk/platform/plt_process.h>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct platform::app_container::state
{
	std::wstring name;
	std::string error;
#ifdef _WIN32
	PSID sid = nullptr;
#endif
};
