#pragma once
#include <thread>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif
namespace core::thread {
	inline void set_name(std::thread& t, const std::string& name) {
#ifdef _WIN32
		std::wstring wname(name.begin(), name.end());
		SetThreadDescription(t.native_handle(), wname.c_str());
#elif defined(__linux__)
		pthread_setname_np(t.native_handle(), name.substr(0, 15).c_str());
#endif
	}

	// Names the calling thread (for pool threads that are not created through std::thread).
	inline void set_current_name(const std::string& name) {
#ifdef _WIN32
		std::wstring wname(name.begin(), name.end());
		SetThreadDescription(GetCurrentThread(), wname.c_str());
#elif defined(__linux__)
		pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
		(void)name;
#endif
	}
}