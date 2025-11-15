#pragma once
#include <common.h>
#include <glm/glm.hpp>
#include "ecs_common_system.h"

namespace app
{
	class application
	{

	public:
		application();
		virtual ~application();
		application(application&&) = delete;
		application& operator= (application&&) = delete;
		application(const application&) = delete;
		application& operator= (const application&) = delete;

		virtual int run();
	private:
		entt::organizer job_organazer;
	};

	application& get_app_system();
}
