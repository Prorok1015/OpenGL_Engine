#pragma once
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_handle.h>
#include <snk/driver/rnd_texture_interface.h>

#include <utility>

namespace rnd::driver
{
	// The sole owner of one texture of a driver: destroys it (driver_interface::destroy) when it goes out of scope.
	// For code that creates a texture and keeps it for a while (the texture cache, placeholders, tests). It holds
	// the driver by pointer, so it must not outlive the driver.
	class texture_owner
	{
	public:
		texture_owner() = default;
		texture_owner(driver_interface& drv, texture_handle handle) noexcept
			: m_driver(&drv)
			, m_handle(handle)
		{}
		texture_owner(const texture_owner&) = delete;
		texture_owner& operator=(const texture_owner&) = delete;
		texture_owner(texture_owner&& other) noexcept
			: m_driver(std::exchange(other.m_driver, nullptr))
			, m_handle(std::exchange(other.m_handle, texture_handle{}))
		{}
		texture_owner& operator=(texture_owner&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_driver = std::exchange(other.m_driver, nullptr);
				m_handle = std::exchange(other.m_handle, texture_handle{});
			}
			return *this;
		}
		~texture_owner() { reset(); }

		texture_handle handle() const noexcept { return m_handle; }
		explicit operator bool() const noexcept { return static_cast<bool>(m_handle); }

		// Destroys the texture now.
		void reset()
		{
			if (m_driver && m_handle) {
				m_driver->destroy(m_handle);
			}
			m_driver = nullptr;
			m_handle = {};
		}

		// Gives the texture up without destroying it.
		texture_handle release() noexcept
		{
			m_driver = nullptr;
			return std::exchange(m_handle, texture_handle{});
		}

	private:
		driver_interface* m_driver = nullptr;
		texture_handle m_handle;
	};

	// The handle of an owned texture, null for none.
	inline texture_handle handle_of(const texture_owner& texture) noexcept
	{
		return texture.handle();
	}

	// Creates a texture and owns it.
	inline texture_owner make_texture(driver_interface& drv, const texture_header& header)
	{
		return texture_owner(drv, drv.create_texture(header));
	}
}
