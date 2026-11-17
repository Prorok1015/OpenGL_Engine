#include "open_gl_specific.h"

#ifdef _DEBUG
#include <glad/glad.h>
#include <string_view>
#include <snk/common/logger/engine_log.h>

void opengl::glCheckError_(const char* file, int line)
{
    constexpr char max_msg_count = 20;

    // __FILE__ is already relative to the source root (see root CMakeLists.txt), so no prefix stripping.
    std::string_view sFile = file;

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
        egLOG_ERROR("opengl", "{}:{} {}", sFile, line, error);

        if (msg_count > max_msg_count) {
            egLOG_ERROR("opengl", "{}:{} Over 20 messages", sFile, line);
            break;
        }
    }
}
#endif