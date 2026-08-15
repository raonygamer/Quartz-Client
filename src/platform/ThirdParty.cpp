#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#undef GLAD_GL_IMPLEMENTATION

#if __has_include(<stb_image.h>)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif
