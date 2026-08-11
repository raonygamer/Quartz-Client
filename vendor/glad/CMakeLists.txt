cmake_minimum_required(VERSION 3.20)

project(glad LANGUAGES C)

add_library(glad INTERFACE)

target_compile_definitions(glad INTERFACE
        GLAD_GL_IMPLEMENTATION
)

target_include_directories(glad INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)