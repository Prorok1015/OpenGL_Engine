#include "rnd_null_swapchain.h"
#include "rnd_null_driver.h"

std::unique_ptr<rnd::driver::driver_interface> rnd::driver::null::swapchain::create_driver()
{
	return std::make_unique<null::driver>();
}
