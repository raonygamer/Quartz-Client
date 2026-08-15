#include <libusb.h>
#include "quartz/rpc/PacketDirection.hpp"
#include "quartz/rpc/PacketHeader.hpp"
#include "quartz/rpc/PacketType.hpp"
#include "quartz/rpc/payloads/FramebufferSetPayload.hpp"
#include "quartz/rpc/payloads/PerformancePayload.hpp"
#include "quartz/rpc/payloads/RowTimingProbePayload.hpp"
#include "quartz/utils/Color32.hpp"

#include <algorithm>
#include <cerrno>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <numeric>
#include <sys/uio.h>
#include <sys/ptrace.h>
#include <sys/user.h>

#include <fcntl.h>
#include <linux/input.h>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <TextEditor.h>

#if __has_include(<Zydis/Zydis.h>)
#include <Zydis/Zydis.h>
#define QUARTZ_HAS_ZYDIS 1
#else
#define QUARTZ_HAS_ZYDIS 0
#endif

#if __has_include(<stb_image.h>)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define QUARTZ_HAS_STB_IMAGE 1
#else
#define QUARTZ_HAS_STB_IMAGE 0
#endif

// clang-format off
#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#undef GLAD_GL_IMPLEMENTATION
#include <GLFW/glfw3.h>
// clang-format on


namespace
{
#include "main/MainShaderSources.inc"
#include "main/MainShaderSources02.inc"
#include "main/MainInput.inc"
#include "main/MainInput02.inc"
#include "main/MainMedia.inc"
#include "main/MainRuntimeModel.inc"
#include "main/MainRuntimeNative.inc"
#include "main/MainRuntimeModel02.inc"
#include "main/MainRuntimeBindings.inc"
#include "main/MainRuntimeControls.inc"
#include "main/MainRuntimeModel03.inc"
#include "main/MainRuntimeBindings02.inc"
#include "main/MainUI.inc"
}

#include "main/MainEntry.inc"
