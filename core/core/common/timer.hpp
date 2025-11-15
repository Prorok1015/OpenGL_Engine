#pragma once
#include <chrono>

struct Timer
{

	static double now() { 
		return std::chrono::duration<double>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch()
			)
		).count();
	}


	static double now_sec() {
		return std::chrono::duration<double>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()
			)
		).count();
	}
	//static double now() { return glfwGetTime(); }
};