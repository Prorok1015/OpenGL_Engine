#pragma once
#include "common.h"

namespace eng {
	class transform3d final
	{
	public:
		transform3d() = default;
		explicit transform3d(glm::mat4 matrix)
		{
			glm::vec3 skew;
			glm::vec4 perspective;

			glm::decompose(matrix, scale, rotation, pos, skew, perspective);
		}
		~transform3d() = default;
		transform3d(const transform3d&) = default;
		transform3d(transform3d&&) = default;
		transform3d& operator= (const transform3d&) = default;
		transform3d& operator= (transform3d&&) = default;

		glm::vec3 forward() const { return glm::rotate(get_rotation(), glm::vec3(0.0f, 0.0f, 1.0f)); }
		glm::vec3 back() const { return -forward(); }
		glm::vec3 right() const { return glm::rotate(get_rotation(), glm::vec3(1.0f, 0.0f, 0.0f)); }
		glm::vec3 left() const { return -right(); }
		glm::vec3 up() const { return glm::rotate(get_rotation(), glm::vec3(0.0f, 1.0f, 0.0f)); }
		glm::vec3 down() const { return -up(); }

		glm::mat4 to_matrix() const {
			return glm::scale(get_scale()) * glm::toMat4(get_rotation()) * glm::translate(get_pos());
		}

		glm::vec3 get_angles_degrees() const {
			return glm::degrees(glm::eulerAngles(rotation));
		}

		float get_pitch() const { return glm::eulerAngles(rotation).x; }
		void add_pitch(float radian) { rotation = glm::normalize(rotation * glm::angleAxis(radian, glm::vec3(1.0f, 0.0f, 0.0f)));  }

		float get_roll() const { return glm::eulerAngles(rotation).z; }
		void add_roll(float radian) { rotation = glm::normalize(rotation * glm::angleAxis(radian, glm::vec3(0.0f, 0.0f, 1.0f))); }

		float get_yaw() const { return glm::eulerAngles(rotation).y; }
		void add_yaw(float radian) { rotation = glm::normalize(rotation * glm::angleAxis(radian, glm::vec3(0.0f, 1.0f, 0.0f))); }


		glm::vec3 get_euler_angles() const {
			return glm::eulerAngles(rotation);
		}

		void set_euler_angles(const glm::vec3& radians)
		{
			rotation = eulerToQuat(radians);
		}

		glm::vec3 get_pos() const { return pos; }
		void set_pos(glm::vec3 val) { pos = val; }

		glm::vec3 get_scale() const { return scale; }
		void set_scale(glm::vec3 val) { scale = val; }

		glm::quat get_rotation() const { return rotation; }
		void set_rotation(glm::quat new_orientation) { rotation = new_orientation; }

		glm::quat eulerToQuat(const glm::vec3& eulerAngles) {
			glm::quat yawQuat = glm::angleAxis(eulerAngles.y, glm::vec3(0.0f, 1.0f, 0.0f));
			glm::quat pitchQuat = glm::angleAxis(eulerAngles.x, glm::vec3(1.0f, 0.0f, 0.0f));
			glm::quat rollQuat = glm::angleAxis(eulerAngles.z, glm::vec3(0.0f, 0.0f, 1.0f));
			return yawQuat * pitchQuat * rollQuat; // YXZ order
		}

	private:
		glm::quat rotation{ glm::vec3{0.f} };
		glm::vec3 scale{ 1 };
		glm::vec3 pos{ 0 };
	};

}