#pragma once
#include <functional>
#include <memory>
#include <utility>

namespace cmd
{
	// Move-only RAII handle of one registry entry: removes the entry when it is destroyed or reset.
	// Stays harmless if the registry dies first (the registry's `alive` token expires).
	class registration
	{
	public:
		registration() = default;
		registration(std::weak_ptr<void> registry_alive, std::function<void()> remove)
			: m_alive(std::move(registry_alive))
			, m_remove(std::move(remove))
		{
		}
		registration(const registration&) = delete;
		registration& operator=(const registration&) = delete;
		registration(registration&& other) noexcept
			: m_alive(std::move(other.m_alive))
			, m_remove(std::exchange(other.m_remove, nullptr))
		{
		}
		registration& operator=(registration&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_alive = std::move(other.m_alive);
				m_remove = std::exchange(other.m_remove, nullptr);
			}
			return *this;
		}
		~registration() { reset(); }

		// False for a default-constructed handle, a moved-from one and a refused registration.
		bool active() const { return m_remove != nullptr; }

		void reset()
		{
			if (auto remove = std::exchange(m_remove, nullptr)) {
				if (m_alive.lock()) remove();
			}
			m_alive.reset();
		}

	private:
		std::weak_ptr<void> m_alive;
		std::function<void()> m_remove;
	};
}
