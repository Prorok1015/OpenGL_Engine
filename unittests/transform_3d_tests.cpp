#include <boost/test/unit_test.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>
#include "common/eng_transform_3d.hpp"
#include <glm/gtc/matrix_transform.hpp>

BOOST_AUTO_TEST_SUITE(Transform3DTests)

BOOST_AUTO_TEST_CASE(DefaultConstructor_InitializesIdentity)
{
    eng::transform3d transform;
    glm::mat4 matrix = transform.to_matrix();
    BOOST_CHECK(matrix == glm::mat4(1.0f));
}

BOOST_AUTO_TEST_CASE(Translate_MovesObject)
{
    eng::transform3d transform;
    transform.set_pos({ 1.0f, 2.0f, 3.0f });
    glm::mat4 matrix = transform.to_matrix();
    glm::mat4 expected = glm::translate(glm::mat4(1.0f), { 1.0f, 2.0f, 3.0f });
    BOOST_CHECK(matrix == expected);
}

BOOST_AUTO_TEST_CASE(Rotate_RotatesObject)
{
    eng::transform3d transform;
    glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    transform.set_rotation(rotation);
    glm::mat4 matrix = transform.to_matrix();
    glm::mat4 expected = glm::toMat4(rotation);
    const float epsilon = 1e-6f;
    for(int i = 0; i < 4; ++i) {
        for(int j = 0; j < 4; ++j) {
            BOOST_CHECK_SMALL(matrix[i][j] - expected[i][j], epsilon);
        }
    }
}

BOOST_AUTO_TEST_CASE(Scale_ScalesObject)
{
    eng::transform3d transform;
    transform.set_scale({ 2.0f, 3.0f, 4.0f });
    glm::mat4 matrix = transform.to_matrix();
    glm::mat4 expected = glm::scale(glm::mat4(1.0f), { 2.0f, 3.0f, 4.0f });
    BOOST_CHECK(matrix == expected);
}

BOOST_AUTO_TEST_CASE(CombinedTransformations_AppliedInCorrectOrder)
{
    eng::transform3d transform;
    transform.set_pos({ 1.0f, 2.0f, 3.0f });
    glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    transform.set_rotation(rotation);
    transform.set_scale({ 2.0f, 3.0f, 4.0f });

    glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), { 2.0f, 3.0f, 4.0f });
    glm::mat4 rotateMat = glm::toMat4(rotation);
    glm::mat4 translateMat = glm::translate(glm::mat4(1.0f), { 1.0f, 2.0f, 3.0f });

    glm::mat4 expected = translateMat * rotateMat * scaleMat;
    glm::mat4 matrix = transform.to_matrix();

    const float epsilon = 1e-6f;
    for(int i = 0; i < 4; ++i) {
        for(int j = 0; j < 4; ++j) {
            BOOST_CHECK_SMALL(matrix[i][j] - expected[i][j], epsilon);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
