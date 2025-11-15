#pragma once
#include <common.h>
#include "inp_key_enums.hpp"

namespace inp
{
	class KeyboardDevice
	{
		struct Hasher {
			std::size_t operator () (KEYBOARD_BUTTONS key) const { return std::size_t(key); }
		};

		using KeyContainer = std::unordered_map<KEYBOARD_BUTTONS, Key, Hasher>;

	public:
		void on_key_action(KEYBOARD_BUTTONS keycode, int scancode, KEY_ACTION action, int mode);
		void on_key_keyboard_action(KEYBOARD_BUTTONS keycode, KEY_ACTION action, float time);

		Key get_key(KEYBOARD_BUTTONS keycode) const { return get_some_key(keycode, keys_); };
		Key get_prev_key(KEYBOARD_BUTTONS keycode) const { return get_some_key(keycode, prev_keys_); };

		template <typename CALLBACK>
		void visit_keys(CALLBACK&& callback) const { std::for_each(keys_.begin(), keys_.end(), [&callback](auto par) { callback(par.first, par.second); }); }

		void clear_key(KEYBOARD_BUTTONS keycode) { prev_keys_[keycode] = keys_[keycode]; keys_[keycode] = {}; }

		bool operator==(const KeyboardDevice& rhs) const noexcept {
			if (keys_.size() != rhs.keys_.size())
				return false;
			if (prev_keys_.size() != rhs.prev_keys_.size())
				return false;
			
			for (const auto& it : rhs.keys_)
			{
				auto itt = keys_.find(it.first);
				if (itt == keys_.end())
					return false;
			}

			for (const auto& it : rhs.prev_keys_)
			{
				auto itt = prev_keys_.find(it.first);
				if (itt != prev_keys_.end())
					return false;
			}

			return true;
		}

	private:
		static Key get_some_key(KEYBOARD_BUTTONS keycode, const KeyContainer& keys);

	public:
		Event<void(KEYBOARD_BUTTONS, KEY_ACTION state)> onKeyStateChanged;

	private:
		KeyContainer keys_;
		KeyContainer prev_keys_;
	};
} 