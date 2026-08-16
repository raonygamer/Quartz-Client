cmake_minimum_required(VERSION 3.23)

project(QuartzClient
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(OpenGL_GL_PREFERENCE GLVND)

set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
set(LIBHAT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL "" FORCE)
set(LIBHAT_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(LIBHAT_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
set(LIBHAT_HINT_X86_64 ON CACHE BOOL "" FORCE)

add_subdirectory(protocol)
add_subdirectory(vendor/glad)
add_subdirectory(vendor/glfw)
add_subdirectory(vendor/libusb)
add_subdirectory(vendor/zydis)
add_subdirectory(vendor/libhat)

add_library(quickjs STATIC
        vendor/quickjs/quickjs.c
        vendor/quickjs/dtoa.c
        vendor/quickjs/libregexp.c
        vendor/quickjs/libunicode.c
        vendor/quickjs/cutils.c
)
target_include_directories(quickjs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION=\"2026-06-04\")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quickjs PRIVATE -fwrapv)
endif()
if(UNIX)
    target_link_libraries(quickjs PUBLIC m)
endif()

file(GLOB_RECURSE QUARTZ_CLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE QUARTZ_CLIENT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

set(QUARTZ_THEME_ASSETS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/QuartzThemeAssets.S")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GENERATE OUTPUT "${QUARTZ_THEME_ASSETS_SOURCE}" CONTENT
".section .rodata.quartz_theme_deviluke,\"a\",@progbits
.balign 16
.global quartz_theme_deviluke_start
.global quartz_theme_deviluke_end
.hidden quartz_theme_deviluke_start
.hidden quartz_theme_deviluke_end
quartz_theme_deviluke_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/deviluke.png\"
quartz_theme_deviluke_end:

.section .rodata.quartz_theme_kurosaki,\"a\",@progbits
.balign 16
.global quartz_theme_kurosaki_start
.global quartz_theme_kurosaki_end
.hidden quartz_theme_kurosaki_start
.hidden quartz_theme_kurosaki_end
quartz_theme_kurosaki_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kurosaki.png\"
quartz_theme_kurosaki_end:

.section .rodata.quartz_theme_yami,\"a\",@progbits
.balign 16
.global quartz_theme_yami_start
.global quartz_theme_yami_end
.hidden quartz_theme_yami_start
.hidden quartz_theme_yami_end
quartz_theme_yami_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/yami.png\"
quartz_theme_yami_end:

.section .rodata.quartz_theme_kirisaki,\"a\",@progbits
.balign 16
.global quartz_theme_kirisaki_start
.global quartz_theme_kirisaki_end
.hidden quartz_theme_kirisaki_start
.hidden quartz_theme_kirisaki_end
quartz_theme_kirisaki_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kirisaki.png\"
quartz_theme_kirisaki_end:

.section .note.GNU-stack,\"\",@progbits
")
set_source_files_properties("${QUARTZ_THEME_ASSETS_SOURCE}" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/deviluke.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kurosaki.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/yami.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kirisaki.png")

add_executable(QuartzClient
        ${QUARTZ_CLIENT_SOURCES}
        ${QUARTZ_THEME_ASSETS_SOURCE}
        vendor/ImGuiColorTextEdit/TextDiff.cpp
        vendor/ImGuiColorTextEdit/TextEditor.cpp
        vendor/imgui/imgui.cpp
        vendor/imgui/imgui_draw.cpp
        vendor/imgui/imgui_tables.cpp
        vendor/imgui/imgui_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_glfw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_opengl3.cpp
        ${QUARTZ_CLIENT_HEADERS}
)

set_target_properties(QuartzClient PROPERTIES
    OUTPUT_NAME quartz
)

target_include_directories(QuartzClient PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
)

target_link_libraries(QuartzClient PRIVATE
        QuartzProtocol
        glad
        glfw
        usb-1.0
        Zydis
        libhat::libhat
        quickjs
)

target_compile_features(QuartzClient PRIVATE cxx_std_20)
