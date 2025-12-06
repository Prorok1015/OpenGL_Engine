#pragma once
#include <glm/glm.hpp>
#include <chrono>
namespace ds
{
	struct performance_timer
	{
		performance_timer() = default;
		~performance_timer() = default;

		void reset()
		{
			start_time = std::chrono::high_resolution_clock::now();
		}

		double elapsed_seconds() const
		{
			auto end_time = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsed = end_time - start_time;
			return elapsed.count();
		}

	private:
		std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
	};

	struct scoped_timer
	{
		scoped_timer(const std::string& name)
			: name(name)
		{
			timer.reset();
		}

		~scoped_timer()
		{
			auto elapsed = timer.elapsed_seconds();
			std::cout << "Timer [" << name << "] elapsed time: " << elapsed << " seconds." << std::endl;
		}

		scoped_timer(const scoped_timer&) = default;
		scoped_timer& operator=(const scoped_timer&) = default;
		scoped_timer(scoped_timer&&) = default;
		scoped_timer& operator=(scoped_timer&&) = default;

	private:
		std::string name;
		performance_timer timer;
	};
}