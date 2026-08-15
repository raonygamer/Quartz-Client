#pragma once
#include <libusb.h>
#include "quartz/rpc/PacketDirection.hpp"
#include "quartz/rpc/PacketHeader.hpp"
#include "quartz/rpc/PacketType.hpp"
#include "quartz/rpc/payloads/FramebufferSetPayload.hpp"
#include "quartz/rpc/payloads/PerformancePayload.hpp"
#include "quartz/rpc/payloads/RowTimingProbePayload.hpp"
#include "quartz/utils/Color32.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
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

#include <fcntl.h>
#include <linux/input.h>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
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
#include <stb_image.h>
#define QUARTZ_HAS_STB_IMAGE 1
#else
#define QUARTZ_HAS_STB_IMAGE 0
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace quartz::client
{
    using quartz::rpc::PacketDirection;
    using quartz::rpc::PacketHeader;
    using quartz::rpc::PacketType;
    using quartz::rpc::payloads::FramebufferSetPayload;
    using quartz::rpc::payloads::MatrixTimingProbeResult;
    using quartz::rpc::payloads::PerformancePayload;
    using quartz::utils::Color32;

    inline constexpr std::uint16_t VendorId = 0xB147;
    inline constexpr std::uint16_t ProductId = 0x4131;
    inline constexpr std::uint8_t ProtocolVersion = 1;
    inline constexpr int RPCInterfaceNumber = 1;
    inline constexpr std::uint8_t RPCOutEndpoint = 0x03;
    inline constexpr std::uint8_t RPCInEndpoint = 0x84;
    inline constexpr const char* FirmwareVersion = "0.0.1.rev1";
    inline constexpr std::size_t Rows = 7;
    inline constexpr std::size_t Columns = 16;
    inline constexpr std::size_t MatrixSize = Rows * Columns;
    inline constexpr std::size_t FFTSize = 512;
    inline constexpr std::size_t ActiveProbeRows = 6;
    inline constexpr int DefaultShaderWidth = static_cast<int>(Columns) * 16;
    inline constexpr int DefaultShaderHeight = static_cast<int>(Rows) * 16;
    inline constexpr int MaxShaderDimension = 4096;
    inline constexpr std::size_t ShaderSourceCapacity = 64 * 1024;
    inline constexpr std::size_t ShaderPathCapacity = 512;
    inline constexpr float Pi = 3.14159265358979323846f;
}
