#pragma once
#ifndef _DEBUG
#define CHECK_GL_ERROR()

#else

namespace opengl {
    // Function to check OpenGL errors and log them
    void glCheckError_(const char* file, int line);
}

// Macro for convenient error checking
#define CHECK_GL_ERROR() opengl::glCheckError_(__FILE__, __LINE__)
#endif