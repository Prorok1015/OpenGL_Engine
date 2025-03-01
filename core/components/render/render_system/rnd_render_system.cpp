#include "rnd_render_system.h"
#include <rnd_driver_interface.h>

rnd::RenderSystem* p_render_system = nullptr;

rnd::RenderSystem& rnd::get_system()
{
	ASSERT_MSG(p_render_system, "Render system is nullptr!");
	return *p_render_system;
}

rnd::RenderSystem::RenderSystem(std::unique_ptr<rnd::driver::driver_interface> driver)
	: drv(std::move(driver))
	, shader_manager(drv.get())
	, texture_manager(drv.get())
	, geom_manager(drv.get())
{
}

void rnd::RenderSystem::activate_renderer(std::weak_ptr<renderer_base> renderer_)
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

void rnd::RenderSystem::deactivate_renderer(std::weak_ptr<renderer_base> renderer)
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

void rnd::RenderSystem::render() const
{
	for (auto& weak_renderer : renderers_list) {
		if (auto renderer = weak_renderer.lock()) {
			renderer->on_render(drv.get());
		}
	}
}
