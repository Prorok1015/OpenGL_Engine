#pragma once
#include <thread>
#include <string>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif
namespace core::thread {
	void set_name(std::thread& t, const std::string& name) {
#ifdef _WIN32
		std::wstring wname(name.begin(), name.end());
		SetThreadDescription(t.native_handle(), wname.c_str());
#elif defined(__linux__)
		pthread_setname_np(t.native_handle(), name.substr(0, 15).c_str());
#endif
	}
}