#include "rnd_render_system.h"
#include <rnd_driver_interface.h>

rnd::render_system* p_render_system = nullptr;

rnd::render_system& rnd::get_system()
{
	ASSERT_MSG(p_render_system, "Render system is nullptr!");
	return *p_render_system;
}

rnd::render_system::render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d)
	: drv(std::move(driver))
	, shader_manager(drv.get())
	, texture_manager(drv.get(), d)
	, geom_manager(drv.get(), d)
{
}

void rnd::render_system::activate_renderer(std::weak_ptr<renderer_base> renderer_)
{
	auto pred = [](auto& lhs, auto& rhs) {
		auto lhs_r = lhs.lock();
		auto rhs_r = rhs.lock();
		if (!lhs_r || !rhs_r) {
			return false;
		}

		return lhs_r->get_render_priority() > rhs_r->get_render_priority();
	};

	renderers_list.insert(std::lower_bound(renderers_list.begin(), renderers_list.end(), renderer_, pred), renderer_);
}

void rnd::render_system::deactivate_renderer(std::weak_ptr<renderer_base> renderer)
{
	auto pred = [find = renderer.lock()](auto& lhs) {
		auto lhs_r = lhs.lock(); 
		if (!lhs_r) {
			return false;
		}

		return lhs_r == find;
	};

	auto it = std::find_if(renderers_list.begin(), renderers_list.end(), pred);
	renderers_list.erase(it);
}

void rnd::render_system::render() const
{
	for (auto& weak_renderer : renderers_list) {
		if (auto renderer = weak_renderer.lock()) {
			renderer->on_render(drv.get());
		}
	}
}
