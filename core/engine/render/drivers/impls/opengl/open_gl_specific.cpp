#include "open_gl_specific.h"

#ifdef _DEBUG
#include <glad/glad.h>
#include <string_view>
#include <engine_log.h>

void opengl::glCheckError_(const char* file, int line)
{
    constexpr int prefix_length = std::string_view(PROJECT_SOURCE_PATH).length() + 1;
    constexpr char max_msg_count = 20;

    std::string_view sFile = file;
    sFile.remove_prefix(prefix_length);

    int msg_count = 0;
    GLenum errorCode = GL_NO_ERROR;
    while ((errorCode = glGetError()) != GL_NO_ERROR)
    {
        ++msg_count;
        std::string error;
        switch (errorCode)
        {
        case GL_INVALID_ENUM:                  error = "INVALID_ENUM"; break;
        case GL_INVALID_VALUE:                 error = "INVALID_VALUE"; break;
        case GL_INVALID_OPERATION:             error = "INVALID_OPERATION"; break;
        case GL_STACK_OVERFLOW:                error = "STACK_OVERFLOW"; break;
        case GL_STACK_UNDERFLOW:               error = "STACK_UNDERFLOW"; break;
        case GL_OUT_OF_MEMORY:                 error = "OUT_OF_MEMORY"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: error = "INVALID_FRAMEBUFFER_OPERATION"; break;
        }
        eg::logger(sFile, line, error, eg::Error{});

        if (msg_count > max_msg_count) {
            eg::logger(sFile, line, "Over 20 masseges", eg::Error{});
            break;
        }
    }
}
#endif