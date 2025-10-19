#pragma once
#include "common.h"
#include "inp_key_enums.hpp"
#include "inp_input_system.h"

namespace inp
{
	class InputActionBase
	{
	public:
		virtual ~InputActionBase() {};
		virtual void update(float dt) = 0;
	};

	template<typename KEY_BUTTON>
	class InputActionClickT : public InputActionBase
	{
	public:
		~InputActionClickT() = default;
		template <typename KB, typename HANDLER>
		static auto create(KB tag, HANDLER&& callback)
		{
			return std::shared_ptr<InputActionClickT<KB>>(new InputActionClickT<KB>(tag, std::forward<HANDLER>(callback)));
		}

		virtual void update(float dt)
		{
			input_system& inpSys = inp::get_system();
			const auto& action = inpSys.get_key_state(button);

			if (action.action == inp::KEY_ACTION::DOWN) {
				action_time += dt;
			}

			if (action.action == inp::KEY_ACTION::UP) {
				if (action_time > 0.f && action_time < START_CLICK_TIME) {
					on_action(dt);
				}
				action_time = 0.f;
			}
		}

	private:
		template <typename HANDLER>
		InputActionClickT(KEY_BUTTON tag, HANDLER&& callback)
			: button(tag)
		{
			on_action += callback;
		}


	public:
		Event<void(float)> on_action;
		KEY_BUTTON button;
		float action_time = 0.f;

		static constexpr float START_CLICK_TIME = 0.2f;
	};

	template<typename KEY_BUTTON>
	class InputAction : public InputActionBase
	{
	public:
		~InputAction() = default;
		template <typename KB, typename HANDLER>
		static auto create(KB tag, HANDLER&& callback)
		{
			return std::shared_ptr<InputAction<KB>>(new InputAction<KB>(tag, std::forward<HANDLER>(callback)));
		}

		virtual void update(float dt)
		{
			input_system& inpSys = inp::get_system();
			const auto& action = inpSys.get_key_state(button);

			if (action.action == inp::KEY_ACTION::DOWN) {
				action_time += dt;
				action_hold_time += dt;

				if (action_time - dt < 0.0001f){
					on_action(dt, inp::KEY_ACTION::DOWN);
				} else if (action_time > STARTS_HOLD_TIME && action_hold_time > REPEAT_HOLD_TIME) {
					action_hold_time = 0.f;
					on_action(dt, inp::KEY_ACTION::NONE);
				}
			}

			if (action.action == inp::KEY_ACTION::UP) {
				action_time = 0.f;
				action_hold_time = -STARTS_HOLD_TIME;
				on_action(dt, inp::KEY_ACTION::UP);
			}
		}
	private:
		template <typename HANDLER>
		InputAction(KEY_BUTTON button_tag, HANDLER&& callback)
			: button(button_tag)
		{
			on_action += callback;
		}

	public:
		Event<void(float, inp::KEY_ACTION)> on_action;
		KEY_BUTTON button;
		float action_time = 0.f;
		float action_hold_time = -STARTS_HOLD_TIME;
		static constexpr float REPEAT_HOLD_TIME = 0.f;
		static constexpr float STARTS_HOLD_TIME = 0.2f;
	};

	template<typename KEY_BUTTON>
	class InputActionHoldT : public InputActionBase
	{
	public:
		~InputActionHoldT() = default;
		template <typename KB, typename HANDLER>
		static auto create(KB tag, HANDLER&& callback)
		{
			return std::shared_ptr<InputActionHoldT<KB>>(new InputActionHoldT<KB>(tag, std::forward<HANDLER>(callback)));
		}

		virtual void update(float dt)
		{
			input_system& inpSys = inp::get_system();
			const auto& action = inpSys.get_key_state(button);
			if (action.action == inp::KEY_ACTION::DOWN) {
				actual_time += dt;
				actual_step_time += dt;

				if (actual_time > activate_time && actual_step_time > step_time) {
					on_action(dt);
					actual_step_time = 0.f;
				}
			}

			if (action.action == inp::KEY_ACTION::UP) {
				actual_time = 0.f;
				actual_step_time = -activate_time;
			}
		}

	private:
		template <typename HANDLER>
		InputActionHoldT(KEY_BUTTON tag, HANDLER&& callback)
			: button(tag)
		{
			on_action += callback;
		}


	public:
		Event<void(float)> on_action;
		KEY_BUTTON button;
		float actual_time = 0.f;
		float actual_step_time = -activate_time;
		static constexpr float step_time = 0.f;
		static constexpr float activate_time = 0.2f;
	};

	class InputActionMouseMove : public InputActionBase
	{
	public:
		~InputActionMouseMove() = default;
		template <typename HANDLER>
		static std::shared_ptr<InputActionMouseMove> create(HANDLER&& callback)
		{
			return std::shared_ptr<InputActionMouseMove>(new InputActionMouseMove(std::forward<HANDLER>(callback)));
		}

		virtual void update(float dt);

	private:
		template <typename HANDLER>
		InputActionMouseMove(HANDLER&& callback) {
			on_action += callback;
		}

	private:
		Event<void(glm::vec2, glm::vec2)> on_action;
		glm::vec2 last_position{ 0.f };
	};

	class InputMouseWhell : public InputActionBase
	{
	public:
		~InputMouseWhell() = default;
		template <typename HANDLER>
		static std::shared_ptr<InputMouseWhell> create(HANDLER&& callback)
		{
			return std::shared_ptr<InputMouseWhell>(new InputMouseWhell(std::forward<HANDLER>(callback)));
		}

		virtual void update(float dt);

	private:
		template <typename HANDLER>
		InputMouseWhell(HANDLER&& callback) {
			on_action += callback;
		}

	private:
		Event<void(glm::vec2, glm::vec2)> on_action;
		glm::vec2 last_position{ 0.f };
	};
}
 