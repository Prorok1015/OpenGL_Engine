#pragma once
#include "ds/ds_type_id.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace inp
{
	struct device_id {
		std::uint32_t type;
		std::uint32_t index = 0;

		auto operator<=>(const device_id& other) const = default;
	};

	constexpr std::size_t MAX_EVENT_DATA_SIZE = 64;
	constexpr std::size_t MAX_EVENT_DATA_ALIGNMENT = 8;

	using storage_buffer = std::aligned_storage_t<MAX_EVENT_DATA_SIZE, MAX_EVENT_DATA_ALIGNMENT>;
	using payload_type_id = std::size_t;

	using copy_func = void(*)(const storage_buffer& src, storage_buffer& dest);
	using move_func = void(*)(storage_buffer& src, storage_buffer& dest);
	using destroy_func = void(*)(storage_buffer& src);

	struct type_metadata {
		copy_func copy;
		move_func move;
		destroy_func destroy;
		payload_type_id type_id;
		std::size_t size;
		std::size_t alignment;
	};

	template<typename T>
		requires std::is_trivial<T>::value
	constexpr type_metadata get_metadata()
	{
		return type_metadata{
			.copy = [](const storage_buffer& src, storage_buffer& dest) {
				new (&dest) T(*reinterpret_cast<const T*>(&src));
			},
			.move = [](storage_buffer& src, storage_buffer& dest) {
				new (&dest) T(std::move(*reinterpret_cast<T*>(&src)));
			},
			.destroy = [](storage_buffer& src) {
				reinterpret_cast<T*>(&src)->~T();
			},
			.type_id = ds::type_id::value<T>(),
			.size = sizeof(T),
			.alignment = alignof(T)
		};
	}

	struct input_event {
	private:
		storage_buffer m_storage{};
		copy_func m_copy_func = nullptr;
		move_func m_move_func = nullptr;
		destroy_func m_destroy_func = nullptr;
		device_id m_device_id;
		payload_type_id m_payload_type_id = 0;

	public:
		input_event() = default;

		~input_event() {
			if (m_destroy_func) {
				m_destroy_func(m_storage);
			}
		}

		input_event(input_event&& other) noexcept
			: m_device_id(other.m_device_id)
			, m_payload_type_id(other.m_payload_type_id)
			, m_copy_func(other.m_copy_func)
			, m_move_func(other.m_move_func)
			, m_destroy_func(other.m_destroy_func)
		{
			if (m_move_func) {
				m_move_func(other.m_storage, m_storage);
			}
			other.m_copy_func = nullptr;
			other.m_move_func = nullptr;
			other.m_destroy_func = nullptr;
		}

		input_event& operator= (input_event&& other) = delete;
		input_event& operator= (const input_event& other) = delete;

		input_event(const input_event& other)
			: m_device_id(other.m_device_id)
			, m_payload_type_id(other.m_payload_type_id)
			, m_copy_func(other.m_copy_func)
			, m_move_func(other.m_move_func)
			, m_destroy_func(other.m_destroy_func)
		{
			if (m_copy_func) {
				m_copy_func(other.m_storage, m_storage);
			}
		}

		const device_id& get_device_id() const { return m_device_id; }
		device_id& get_device_id() { return m_device_id; }
		payload_type_id get_payload_type_id() const { return m_payload_type_id; }

		template<typename T, typename... Args>
		void construct_payload(Args&&... args) {
			const type_metadata meta = get_metadata<T>();

			static_assert(sizeof(T) <= MAX_EVENT_DATA_SIZE, "Payload size exceeds fixed buffer!");

			if (meta.size > MAX_EVENT_DATA_SIZE) {
				return;
			}

			if (m_destroy_func) m_destroy_func(m_storage);

			m_copy_func = meta.copy;
			m_move_func = meta.move;
			m_destroy_func = meta.destroy;
			m_payload_type_id = meta.type_id;

			new (&m_storage) T(std::forward<Args>(args)...);
		}

		template<typename T>
		const T* get_payload() const {
			if (get_metadata<T>().type_id != m_payload_type_id) {
				return nullptr;
			}

			return input_event::to_type<T>(m_storage);
		}

	private:
		template<typename T>
		static T* to_type(storage_buffer& buffer) { return reinterpret_cast<T*>(&buffer); }
		template<typename T>
		static const T* to_type(const storage_buffer& buffer) { return reinterpret_cast<const T*>(&buffer); }
	};
}