#pragma once
#include "common.h"
namespace wnd
{
	enum class KEY_ACTION : uint8_t
	{
		NONE,
		UP,
		DOWN,
	};

	enum class KEYBOARD_BUTTONS : uint8_t
	{
		A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		SPACE, APOSTROPHE, COMMA, MINUS, PERIOD, SLASH,
		K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
		SEMICOLON, EQUAL,
		ESCAPE, ENTER, TAB, BACKSPACE, INSERT, DELETE,
		RIGHT, LEFT, DOWN, UP, PAGE_UP, PAGE_DOWN, HOME, END,
		CAPS_LOCK, SCROLL_LOCK, NUM_LOCK, PRINT_SCREEN, PAUSE,
		F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
		KP_0, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9, KP_DECIMAL, KP_DIVIDE, KP_MULTIPLY, KP_SUBTRACT, KP_ADD, KP_ENTER, KP_EQUAL,
		LEFT_SHIFT, LEFT_CONTROL, LEFT_ALT, LEFT_SUPER, RIGHT_SHIFT, RIGHT_CONTROL, RIGHT_ALT, RIGHT_SUPER, MENU
	};

	enum class MOUSE_BUTTONS : uint8_t
	{
		LEFT,
		RIGHT,
		MIDDLE,

		BTN_1 = LEFT,
		BTN_2 = RIGHT,
		BTN_3 = MIDDLE,
		BTN_4,
		BTN_5,
		BTN_6,
		BTN_7,
		BTN_8,
		LAST,
		COUNT
	};

	using handle = void*;

	struct device_id {
		std::uint32_t type;
		std::uint32_t index = 0;

		auto operator<=>(const device_id& other) const = default;
	};

	constexpr size_t MAX_EVENT_DATA_SIZE = 64;
	constexpr size_t MAX_EVENT_DATA_ALIGNMENT = 8;

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
		size_t size;
		size_t alignment;
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

	public:
		device_id device_id;
		payload_type_id payload_type_id = 0;

		input_event() = default;

		~input_event() {
			if (m_destroy_func) {
				m_destroy_func(m_storage);
			}
		}

		input_event(input_event&& other) noexcept
			: device_id(other.device_id)
			, payload_type_id(other.payload_type_id)
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
			: device_id(other.device_id)
			, payload_type_id(other.payload_type_id)
			, m_copy_func(other.m_copy_func)
			, m_move_func(other.m_move_func)
			, m_destroy_func(other.m_destroy_func)
		{
			if (m_copy_func) {
				m_copy_func(other.m_storage, m_storage);
			}
		}

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
			payload_type_id = meta.type_id;

			new (&m_storage) T(std::forward<Args>(args)...);
		}

		template<typename T>
		const T* get_payload() const {
			if (get_metadata<T>().type_id != payload_type_id) {
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