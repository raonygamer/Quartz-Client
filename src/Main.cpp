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
    using quartz::rpc::PacketDirection;
    using quartz::rpc::PacketHeader;
    using quartz::rpc::PacketType;
    using quartz::rpc::payloads::FramebufferSetPayload;
    using quartz::rpc::payloads::MatrixTimingProbeResult;
    using quartz::rpc::payloads::PerformancePayload;
    using quartz::utils::Color32;

    constexpr std::uint16_t VendorId = 0xB147;
    constexpr std::uint16_t ProductId = 0x4131;
    constexpr std::uint8_t ProtocolVersion = 1;
    constexpr int RPCInterfaceNumber = 1;
    constexpr std::uint8_t RPCOutEndpoint = 0x03;
    constexpr std::uint8_t RPCInEndpoint = 0x84;
    constexpr const char* FirmwareVersion = "0.0.1.rev1";
    constexpr std::size_t Rows = 7;
    constexpr std::size_t Columns = 16;
    constexpr std::size_t MatrixSize = Rows * Columns;
    constexpr std::size_t FFTSize = 512;
    constexpr std::size_t ActiveProbeRows = 6;
    constexpr int DefaultShaderWidth = static_cast<int>(Columns) * 16;
    constexpr int DefaultShaderHeight = static_cast<int>(Rows) * 16;
    constexpr int MaxShaderDimension = 4096;
    constexpr std::size_t ShaderSourceCapacity = 64 * 1024;
    constexpr std::size_t ShaderPathCapacity = 512;
    constexpr float Pi = 3.14159265358979323846f;

    constexpr std::string_view DefaultVertexShaderSource = R"GLSL(#version 330 core
out vec2 vUV;
void main()
{
    const vec2 positions[6] = vec2[6](
        vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
        vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
    );
    vec2 position = positions[gl_VertexID];
    vUV = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL";

    constexpr std::string_view LegacyDefaultFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform float uBands[16];
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform vec3 uSolidColor;
uniform float uWaveSpeed;
uniform float uFeatherRows;
uniform float uSaturation;
uniform int uForceFullRow;
uniform int uFullRow;

vec3 hsv2rgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

vec3 saturateColor(vec3 color, float amount)
{
    float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return clamp(vec3(gray) + (color - vec3(gray)) * amount, 0.0, 1.0);
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    // Raw framebuffer row 6 is the unused extra row. The real keyboard occupies rows 0..5.
    if (logicalRow == 6) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float physicalY = clamp((vUV.y - 1.0 / 7.0) / (6.0 / 7.0), 0.0, 1.0);
    float level = clamp(uBands[column], 0.0, 1.0);
    float feather = max(uFeatherRows / 6.0, 0.001);
    float amount = 1.0 - smoothstep(level, level + feather, physicalY);
    if (uForceFullRow != 0 && logicalRow == uFullRow)
        amount = 1.0;

    vec3 wave = hsv2rgb(vec3(fract(vUV.x - uTime * uWaveSpeed), 1.0, 1.0));
    vec3 color = mix(wave, uMediaColor, uMediaAmount);
    color = saturateColor(color, uSaturation);
    FragColor = vec4(color * amount, 1.0);
}
)GLSL";

    constexpr std::string_view DefaultFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform float uBands[16];
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform vec3 uSolidColor;
uniform float uWaveSpeed;
uniform float uFeatherRows;
uniform float uSaturation;
uniform int uForceFullRow;
uniform int uFullRow;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

vec3 saturateColor(vec3 color, float amount)
{
    float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return clamp(vec3(gray) + (color - vec3(gray)) * amount, 0.0, 1.0);
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    // The real keyboard occupies rows 0..5. Row 6 is the spare logical row.
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float level = clamp(uBands[column], 0.0, 1.0);
    float visualRow = float(5 - logicalRow);
    float exactRows = level * 6.0;
    float amount = clamp((exactRows - visualRow) / max(uFeatherRows, 0.001), 0.0, 1.0);
    amount = amount * amount * (3.0 - 2.0 * amount);

    if (uForceFullRow != 0 && logicalRow == uFullRow)
        amount = 1.0;

    vec3 wave = hsv2rgb(vec3(fract(vUV.x - uTime * uWaveSpeed), 1.0, 1.0));
    vec3 color = mix(wave, uMediaColor, uMediaAmount);
    color = saturateColor(color, uSaturation);

    // uCapsLock/uScrollLock are the OS-maintained lock LED states from Linux evdev.
    FragColor = vec4(applyKeyIndicators(color * amount), 1.0);
}
)GLSL";

    constexpr std::string_view PlasmaFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;
uniform float uPatternSpeed; // @ui min=0.0 max=3.0 step=0.01 default=0.9 label="Pattern speed" id=plasma.speed
uniform float uPatternBrightness; // @ui min=0.0 max=2.0 step=0.01 default=1.0 label="Brightness" id=plasma.brightness

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = vUV * 6.0 - 3.0;
    float t = uTime * uPatternSpeed;
    float value = sin(p.x * 1.7 + t)
        + sin(p.y * 2.1 - t * 1.3)
        + sin((p.x + p.y) * 1.4 + t * 0.7)
        + sin(length(p) * 2.4 - t * 1.8);
    value = value * 0.125 + 0.5;

    vec3 color = hsv2rgb(vec3(fract(value + t * 0.035), 0.95, 1.0));
    color = mix(color, uMediaColor, uMediaAmount * 0.55);
    FragColor = vec4(applyKeyIndicators(clamp(color * uPatternBrightness, 0.0, 1.0)), 1.0);
}
)GLSL";

    constexpr std::string_view NeonRingsFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float distanceToCenter = length(p);
    float ring = pow(1.0 - abs(sin(distanceToCenter * 24.0 - uTime * 3.0)), 10.0);
    float glow = 0.15 / (0.04 + abs(fract(distanceToCenter * 5.0 - uTime * 0.25) - 0.5));
    vec3 color = hsv2rgb(vec3(fract(distanceToCenter * 1.8 - uTime * 0.08), 0.95, 1.0)) * (ring + glow * 0.14);
    color = mix(color, uMediaColor * max(length(color), 0.25), uMediaAmount * 0.5);
    FragColor = vec4(applyKeyIndicators(clamp(color, 0.0, 1.0)), 1.0);
}
)GLSL";

    constexpr std::string_view RotatingBoxesFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

mat2 rotate2d(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    p = rotate2d(uTime * 0.35) * p;
    vec2 grid = fract(p * 7.0) - 0.5;
    float box = max(abs(grid.x), abs(grid.y));
    float fill = 1.0 - smoothstep(0.34, 0.43, box);
    float line = 1.0 - smoothstep(0.43, 0.48, abs(box - 0.40));
    vec3 color = hsv2rgb(vec3(fract(p.x + p.y + uTime * 0.05), 0.9, 1.0)) * (fill * 0.35 + line);
    color = mix(color, uMediaColor, uMediaAmount * fill * 0.45);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view CheckerWarpFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = vUV;
    p.x += sin(p.y * 12.0 + uTime * 2.0) * 0.06;
    p.y += sin(p.x * 10.0 - uTime * 1.7) * 0.05;
    vec2 cell = floor(p * vec2(16.0, 8.0));
    float checker = mod(cell.x + cell.y, 2.0);
    vec3 colorA = vec3(0.03, 0.70, 1.00);
    vec3 colorB = vec3(1.00, 0.05, 0.55);
    vec3 color = mix(colorA, colorB, checker);
    float pulse = 0.55 + 0.45 * sin((p.x + p.y) * 8.0 - uTime * 2.2);
    color *= 0.55 + 0.45 * pulse;
    color = mix(color, uMediaColor, uMediaAmount * 0.5);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view DiamondTunnelFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float angle = atan(p.y, p.x);
    float radius = length(p);
    float tunnel = fract(5.0 * radius - uTime * 0.9);
    float diamond = abs(fract((angle / 6.2831853) * 8.0 + uTime * 0.08) - 0.5) * 2.0;
    float edge = 1.0 - smoothstep(0.04, 0.12, abs(tunnel - diamond * 0.35));
    vec3 color = hsv2rgb(vec3(fract(radius * 2.0 + angle / 6.2831853 - uTime * 0.08), 1.0, 1.0)) * edge;
    color += hsv2rgb(vec3(fract(angle / 6.2831853), 0.8, 0.25)) * 0.18;
    color = mix(color, uMediaColor, uMediaAmount * edge * 0.45);
    FragColor = vec4(applyKeyIndicators(clamp(color, 0.0, 1.0)), 1.0);
}
)GLSL";

    constexpr std::string_view WaveInterferenceFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0) * 7.0;
    vec2 sourceA = vec2(sin(uTime), cos(uTime)) * 1.6;
    vec2 sourceB = vec2(cos(uTime * 0.7), sin(uTime * 0.9)) * 1.8;
    float wave = sin(length(p - sourceA) * 5.0 - uTime * 3.0)
        + sin(length(p + sourceB) * 5.5 - uTime * 2.6);
    float value = 0.5 + 0.5 * sin(wave * 2.3);
    vec3 color = hsv2rgb(vec3(fract(value * 0.3 + uTime * 0.04), 0.95, value));
    color = mix(color, uMediaColor, uMediaAmount * 0.45);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view RadialPulseFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float distanceToCenter = length(p);
    float angle = atan(p.y, p.x) / 6.2831853;
    float pulse = 0.45 + 0.55 * sin(distanceToCenter * 28.0 - uTime * 4.0);
    pulse = smoothstep(-0.2, 1.0, pulse);
    vec3 color = hsv2rgb(vec3(fract(angle + distanceToCenter * 0.8 - uTime * 0.06), 0.95, pulse));
    color = mix(color, uMediaColor, uMediaAmount * 0.55);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view SpectrumFireFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform float uBands[16];
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform int uForceFullRow;
uniform int uFullRow;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float visualRow = float(5 - logicalRow);
    float heat = clamp(uBands[column] * 6.0 - visualRow, 0.0, 1.0);
    if (uForceFullRow != 0 && logicalRow == uFullRow)
        heat = 1.0;

    float flicker = 0.88 + 0.12 * sin(float(column) * 2.1 + uTime * 12.0);
    heat *= flicker;
    vec3 color = mix(vec3(0.22, 0.0, 0.0), vec3(1.0, 0.12, 0.0), heat);
    color = mix(color, vec3(1.0, 0.95, 0.25), heat * heat);
    color *= heat;
    color = mix(color, uMediaColor * heat, uMediaAmount * 0.45);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view KaleidoscopeFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float radius = length(p);
    float angle = atan(p.y, p.x) + uTime * 0.25;
    float sectors = 8.0;
    angle = abs(mod(angle, 6.2831853 / sectors) - 3.14159265 / sectors);
    vec2 q = vec2(cos(angle), sin(angle)) * radius;
    q = fract(q * 7.0) - 0.5;
    float triangle = max(abs(q.x) * 0.866025 + q.y * 0.5, -q.y);
    float edge = 1.0 - smoothstep(0.28, 0.36, abs(triangle - 0.24));
    vec3 color = hsv2rgb(vec3(fract(radius * 1.7 + angle * 0.5 - uTime * 0.05), 0.95, 0.25 + 0.75 * edge));
    color = mix(color, uMediaColor, uMediaAmount * edge * 0.5);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
}
)GLSL";

    constexpr std::string_view GeometricGridFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

mat2 rotate2d(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = (vUV - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    p = rotate2d(sin(uTime * 0.35) * 0.45) * p;
    vec2 cell = fract(p * 9.0 + 0.5) - 0.5;
    float circle = abs(length(cell) - 0.27);
    float cross = min(abs(cell.x), abs(cell.y));
    float shape = min(circle, cross * 0.8);
    float glow = 0.018 / (0.012 + shape);
    vec3 color = hsv2rgb(vec3(fract(p.x * 0.4 - p.y * 0.25 + uTime * 0.04), 0.9, 1.0)) * glow;
    color = mix(color, uMediaColor, uMediaAmount * min(glow, 1.0) * 0.45);
    FragColor = vec4(applyKeyIndicators(clamp(color, 0.0, 1.0)), 1.0);
}
)GLSL";

    constexpr std::string_view RotatingCubeFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyKeyIndicators(vec3 color)
{
    int keyColumn = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int keyRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && keyRow == 3 && keyColumn == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && keyRow == 0 && keyColumn == 14)
        return uScrollLockColor;
    return color;
}

mat2 rotate2d(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

float boxSdf(vec3 p, vec3 halfSize)
{
    vec3 q = abs(p) - halfSize;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

vec3 cubeNormal(vec3 p)
{
    const float epsilon = 0.002;
    return normalize(vec3(
        boxSdf(p + vec3(epsilon, 0.0, 0.0), vec3(0.72)) - boxSdf(p - vec3(epsilon, 0.0, 0.0), vec3(0.72)),
        boxSdf(p + vec3(0.0, epsilon, 0.0), vec3(0.72)) - boxSdf(p - vec3(0.0, epsilon, 0.0), vec3(0.72)),
        boxSdf(p + vec3(0.0, 0.0, epsilon), vec3(0.72)) - boxSdf(p - vec3(0.0, 0.0, epsilon), vec3(0.72))
    ));
}

vec3 rotateCube(vec3 p, float time)
{
    p.xz = rotate2d(time * 0.73) * p.xz;
    p.yz = rotate2d(time * 0.51) * p.yz;
    p.xy = rotate2d(time * 0.29) * p.xy;
    return p;
}

void main()
{
    int logicalRow = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (logicalRow == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float aspect = uResolution.x / max(uResolution.y, 1.0);
    vec2 screen = (vUV - 0.5) * vec2(aspect, 1.0);
    vec3 rayOrigin = vec3(0.0, 0.0, -3.4);
    vec3 rayDirection = normalize(vec3(screen * 1.65, 1.75));

    float distanceTravelled = 0.0;
    bool hit = false;
    vec3 localPoint = vec3(0.0);
    for (int step = 0; step < 64; ++step)
    {
        vec3 worldPoint = rayOrigin + rayDirection * distanceTravelled;
        localPoint = rotateCube(worldPoint, uTime);
        float distanceToCube = boxSdf(localPoint, vec3(0.72));
        if (distanceToCube < 0.002)
        {
            hit = true;
            break;
        }
        distanceTravelled += distanceToCube * 0.72;
        if (distanceTravelled > 7.0)
            break;
    }

    vec3 background = vec3(0.006, 0.008, 0.015);
    float gridX = 1.0 - smoothstep(0.0, 0.025, abs(fract(vUV.x * 16.0) - 0.5));
    float gridY = 1.0 - smoothstep(0.0, 0.040, abs(fract(vUV.y * 7.0) - 0.5));
    background += vec3(0.015, 0.025, 0.045) * max(gridX, gridY);

    vec3 color = background;
    if (hit)
    {
        vec3 normal = cubeNormal(localPoint);
        vec3 lightDirection = normalize(vec3(-0.5, 0.8, -0.7));
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float rim = pow(1.0 - max(dot(normalize(-rayDirection), normal), 0.0), 2.5);
        vec3 faceColor = 0.45 + 0.45 * normal;
        faceColor = mix(faceColor, uMediaColor, uMediaAmount * 0.45);
        color = faceColor * (0.22 + diffuse * 0.88) + vec3(0.12, 0.42, 1.0) * rim;
    }

    FragColor = vec4(applyKeyIndicators(clamp(color, 0.0, 1.0)), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveKeyGlowFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uKeyState[112];
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float held = uKeyState[row * 16 + column];
    float afterglow = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5 || int(event.x) != column || int(event.y) != row)
            continue;
        afterglow = max(afterglow, exp(-max(uTime - event.z, 0.0) * 5.0));
    }

    float intensity = max(held, afterglow);
    vec3 idle = vec3(0.004, 0.008, 0.014);
    vec3 activeColor = mix(vec3(0.05, 0.55, 1.0), vec3(0.75, 0.95, 1.0), held);
    FragColor = vec4(applyIndicators(mix(idle, activeColor, intensity), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveRippleFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uKeyState[112];
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float wave = 0.0;
    vec2 aspect = vec2(16.0 / 7.0, 1.0);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.2)
            continue;
        float distanceToEvent = length((vUV - eventPoint(event)) * aspect);
        float radius = age * 0.65;
        float ring = exp(-pow((distanceToEvent - radius) * 28.0, 2.0));
        wave += ring * exp(-age * 1.2);
    }

    float held = uKeyState[row * 16 + column];
    vec3 color = vec3(0.005, 0.010, 0.018) + vec3(0.02, 0.45, 1.0) * wave + vec3(0.75, 0.95, 1.0) * held;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveRainbowRippleFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.003, 0.004, 0.008);
    vec2 aspect = vec2(16.0 / 7.0, 1.0);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.6)
            continue;
        float distanceToEvent = length((vUV - eventPoint(event)) * aspect);
        float radius = age * 0.58;
        float ring = exp(-pow((distanceToEvent - radius) * 32.0, 2.0)) * exp(-age * 0.9);
        vec3 ringColor = hsv2rgb(vec3(fract(event.x / 16.0 + event.y / 14.0 + age * 0.12), 0.95, 1.0));
        color += ringColor * ring;
    }

    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveHeatFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uKeyState[112];
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float heat = uKeyState[row * 16 + column];
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5 || int(event.x) != column || int(event.y) != row)
            continue;
        heat = max(heat, exp(-max(uTime - event.z, 0.0) * 1.8));
    }

    vec3 cold = vec3(0.002, 0.004, 0.010);
    vec3 red = vec3(0.85, 0.015, 0.0);
    vec3 yellow = vec3(1.0, 0.75, 0.04);
    vec3 white = vec3(1.0, 0.98, 0.82);
    vec3 color = mix(cold, red, smoothstep(0.0, 0.45, heat));
    color = mix(color, yellow, smoothstep(0.35, 0.75, heat));
    color = mix(color, white, smoothstep(0.75, 1.0, heat));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveCrossBlastFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.003, 0.006, 0.012);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.6)
            continue;
        vec2 point = eventPoint(event);
        float horizontal = exp(-abs(vUV.y - point.y) * 80.0);
        float vertical = exp(-abs(vUV.x - point.x) * 80.0);
        float travelX = exp(-pow((abs(vUV.x - point.x) - age * 0.55) * 18.0, 2.0));
        float travelY = exp(-pow((abs(vUV.y - point.y) - age * 0.55) * 18.0, 2.0));
        float beam = (horizontal * travelX + vertical * travelY) * exp(-age * 1.4);
        color += vec3(0.12, 0.75, 1.0) * beam;
    }

    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveSparksFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

float hash11(float value)
{
    return fract(sin(value * 91.3458) * 47453.5453);
}

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.002, 0.003, 0.008);
    vec2 aspect = vec2(16.0 / 7.0, 1.0);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.8)
            continue;
        vec2 origin = eventPoint(event);
        for (int spark = 0; spark < 4; ++spark)
        {
            float seed = event.x * 13.0 + event.y * 31.0 + float(spark) * 7.0;
            float direction = mix(-0.34, 0.34, hash11(seed));
            float speed = mix(0.18, 0.42, hash11(seed + 1.0));
            vec2 point = origin + vec2(direction * age, speed * age - 0.26 * age * age);
            float distanceToSpark = length((vUV - point) * aspect);
            float glow = 0.010 / (0.004 + distanceToSpark * distanceToSpark * 65.0);
            color += mix(vec3(1.0, 0.15, 0.02), vec3(1.0, 0.9, 0.25), hash11(seed + 2.0)) * glow * exp(-age * 1.6);
        }
    }

    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveManhattanFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.004, 0.005, 0.012);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.2)
            continue;
        float gridDistance = abs(float(column) - event.x) + abs(float(row) - event.y);
        float front = age * 7.0;
        float pulse = exp(-pow((gridDistance - front) * 1.4, 2.0)) * exp(-age * 0.8);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + age * 0.08), 0.9, 1.0)) * pulse;
    }

    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveVortexFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 hsv2rgb(vec3 color)
{
    vec3 p = abs(fract(color.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return color.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), color.y);
}

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 p = vUV;
    float energy = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.5)
            continue;
        vec2 center = eventPoint(event);
        vec2 delta = p - center;
        float radius = length(delta);
        float angle = atan(delta.y, delta.x) + exp(-radius * 7.0) * age * 7.0;
        p = center + vec2(cos(angle), sin(angle)) * radius;
        energy += exp(-radius * 7.0) * exp(-age * 0.9);
    }

    float pattern = 0.5 + 0.5 * sin(p.x * 35.0 + p.y * 19.0 - uTime * 2.0);
    vec3 color = hsv2rgb(vec3(fract(p.x * 0.8 + p.y * 0.3 + uTime * 0.03), 0.9, 0.08 + pattern * min(energy, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveStarburstFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec2 eventPoint(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 7.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.002, 0.003, 0.009);
    vec2 aspect = vec2(16.0 / 7.0, 1.0);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.7)
            continue;
        vec2 delta = (vUV - eventPoint(event)) * aspect;
        float radius = length(delta);
        float angle = atan(delta.y, delta.x);
        float rays = pow(abs(cos(angle * 6.0 + age * 3.0)), 18.0);
        float core = 0.012 / (0.006 + radius * radius * 50.0);
        float burst = rays * exp(-radius * 7.0) * exp(-age * 1.6);
        color += vec3(0.35, 0.65, 1.0) * (core + burst);
    }

    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    constexpr std::string_view ReactiveScannerFragmentShaderSource = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uKeyState[112];
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 color = vec3(0.003, 0.007, 0.009);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5)
            continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.0)
            continue;
        float leftFront = event.x - age * 10.0;
        float rightFront = event.x + age * 10.0;
        float scan = exp(-abs(float(column) - leftFront) * 1.8) + exp(-abs(float(column) - rightFront) * 1.8);
        float rowFalloff = exp(-abs(float(row) - event.y) * 0.55);
        color += vec3(0.10, 1.0, 0.65) * scan * rowFalloff * exp(-age * 1.1);
    }

    float held = uKeyState[row * 16 + column];
    color += vec3(0.65, 1.0, 0.82) * held;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);
}
)GLSL";

    struct ShaderPreset
    {
        std::string Name;
        std::string FragmentSource;
    };

    static std::string makeGeneratedShader(const std::string_view body)
    {
        static constexpr std::string_view Header = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec2 uResolution;
uniform float uBands[16];
uniform float uKeyState[112];
uniform vec4 uKeyEvents[16];
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

vec3 hsv2rgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

float hash11(float p)
{
    return fract(sin(p * 127.1) * 43758.5453123);
}

float hash21(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

mat2 rotate2d(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

float sdEquilateralTriangle(vec2 p, float radius)
{
    const float k = 1.7320508;
    p.x = abs(p.x) - radius;
    p.y = p.y + radius / k;
    if (p.x + k * p.y > 0.0)
        p = vec2(p.x - k * p.y, -k * p.x - p.y) * 0.5;
    p.x -= clamp(p.x, -2.0 * radius, 0.0);
    return -length(p) * sign(p.y);
}

vec2 eventUv(vec4 event)
{
    return vec2((event.x + 0.5) / 16.0, 1.0 - (event.y + 0.5) / 6.0);
}

vec3 applyIndicators(vec3 color, int row, int column)
{
    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        return uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        return uScrollLockColor;
    return color;
}

void main()
{
    int column = clamp(int(floor(vUV.x * 16.0)), 0, 15);
    int row = clamp(int(floor((1.0 - vUV.y) * 7.0)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 uv = vec2(vUV.x, clamp((vUV.y - 1.0 / 7.0) / (6.0 / 7.0), 0.0, 1.0));
    vec2 p = (uv - 0.5) * vec2(2.5, 1.0);
)GLSL";
        std::string source(Header);
        source.append(body);
        source.append("\n}\n");
        return source;
    }

    static std::string shaderPresetId(const std::string_view name)
    {
        std::string result;
        result.reserve(name.size());
        for (const unsigned char c : name)
        {
            if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
            else if (!result.empty() && result.back() != '_') result += '_';
        }
        while (!result.empty() && result.back() == '_') result.pop_back();
        return result.empty() ? "preset" : result;
    }

    static void parameterizeShaderPreset(ShaderPreset& preset)
    {
        auto& source = preset.FragmentSource;
        const std::string id = shaderPresetId(preset.Name);
        const std::size_t output = source.find("out vec4 FragColor;");
        if (output == std::string::npos) return;
        const std::size_t insertion = source.find('\n', output);
        if (insertion == std::string::npos) return;

        std::string declarations;
        if (source.find("uMaterialBrightness") == std::string::npos)
            declarations += "\nuniform float uMaterialBrightness; // @ui min=0.0 max=2.0 step=0.01 default=1.0 label=\"Brightness\" id=" + id + ".brightness";
        if (source.find("uMaterialSaturation") == std::string::npos)
            declarations += "\nuniform float uMaterialSaturation; // @ui min=0.0 max=2.0 step=0.01 default=1.0 label=\"Saturation\" id=" + id + ".saturation";
        if (source.find("uMaterialContrast") == std::string::npos)
            declarations += "\nuniform float uMaterialContrast; // @ui min=0.0 max=2.0 step=0.01 default=1.0 label=\"Contrast\" id=" + id + ".contrast";
        const std::size_t timeDeclaration = source.find("uniform float uTime;");
        if (timeDeclaration != std::string::npos && source.find("uMaterialTimeScale") == std::string::npos)
        {
            declarations += "\nuniform float uMaterialTimeScale; // @ui min=-3.0 max=3.0 step=0.01 default=1.0 label=\"Animation speed\" id=" + id + ".speed";
            const std::size_t afterTime = source.find('\n', timeDeclaration);
            if (afterTime != std::string::npos)
            {
                std::size_t position = afterTime + 1;
                while ((position = source.find("uTime", position)) != std::string::npos)
                {
                    source.replace(position, 5, "uMaterialTime");
                    position += 13;
                }
                declarations += "\n#define uMaterialTime (uTime * uMaterialTimeScale)";
            }
        }
        source.insert(insertion + 1, declarations + "\n");

        const std::size_t finalBrace = source.find_last_of('}');
        if (finalBrace != std::string::npos)
            source.insert(finalBrace, "    float uMaterialLuma = dot(FragColor.rgb, vec3(0.2126, 0.7152, 0.0722));\n    FragColor.rgb = mix(vec3(uMaterialLuma), FragColor.rgb, uMaterialSaturation);\n    FragColor.rgb = (FragColor.rgb - vec3(0.5)) * uMaterialContrast + vec3(0.5);\n    FragColor.rgb = clamp(FragColor.rgb * uMaterialBrightness, 0.0, 1.0);\n");
    }

    static std::string makeSourceShader(const std::string_view declaration, const std::string_view body)
    {
        std::string source = makeGeneratedShader(body);
        const std::size_t marker = source.find("uniform float uTime;");
        if (marker != std::string::npos) source.insert(marker, std::string(declaration) + "\n");
        return source;
    }

    static std::vector<ShaderPreset> buildShaderPresets()
    {
        std::vector<ShaderPreset> presets;
        presets.reserve(96);
        presets.push_back({"Rainbow equalizer", std::string(DefaultFragmentShaderSource)});
        presets.push_back({"Plasma", std::string(PlasmaFragmentShaderSource)});
        presets.push_back({"Neon rings", std::string(NeonRingsFragmentShaderSource)});
        presets.push_back({"Rotating boxes", std::string(RotatingBoxesFragmentShaderSource)});
        presets.push_back({"Checker warp", std::string(CheckerWarpFragmentShaderSource)});
        presets.push_back({"Diamond tunnel", std::string(DiamondTunnelFragmentShaderSource)});
        presets.push_back({"Wave interference", std::string(WaveInterferenceFragmentShaderSource)});
        presets.push_back({"Radial pulse", std::string(RadialPulseFragmentShaderSource)});
        presets.push_back({"Spectrum fire", std::string(SpectrumFireFragmentShaderSource)});
        presets.push_back({"Kaleidoscope", std::string(KaleidoscopeFragmentShaderSource)});
        presets.push_back({"Geometric grid", std::string(GeometricGridFragmentShaderSource)});
        presets.push_back({"Rotating cube", std::string(RotatingCubeFragmentShaderSource)});
        presets.push_back({"Reactive - key glow", std::string(ReactiveKeyGlowFragmentShaderSource)});
        presets.push_back({"Reactive - ripple", std::string(ReactiveRippleFragmentShaderSource)});
        presets.push_back({"Reactive - rainbow ripple", std::string(ReactiveRainbowRippleFragmentShaderSource)});
        presets.push_back({"Reactive - heat", std::string(ReactiveHeatFragmentShaderSource)});
        presets.push_back({"Reactive - cross blast", std::string(ReactiveCrossBlastFragmentShaderSource)});
        presets.push_back({"Reactive - sparks", std::string(ReactiveSparksFragmentShaderSource)});
        presets.push_back({"Reactive - Manhattan wave", std::string(ReactiveManhattanFragmentShaderSource)});
        presets.push_back({"Reactive - vortex", std::string(ReactiveVortexFragmentShaderSource)});
        presets.push_back({"Reactive - starburst", std::string(ReactiveStarburstFragmentShaderSource)});
        presets.push_back({"Reactive - scanner", std::string(ReactiveScannerFragmentShaderSource)});
        presets.push_back({"Aurora curtains", makeGeneratedShader(R"GLSL(    float curtain = 0.5 + 0.5 * sin(10.0 * uv.x + sin(uv.y * 7.0 + uTime) * 2.5 + uTime * 1.4);
    float glow = pow(curtain, 5.0) * (0.35 + 0.65 * sin(uv.y * 3.14159));
    vec3 color = mix(vec3(0.01, 0.03, 0.06), hsv2rgb(vec3(0.42 + uv.x * 0.18 + uTime * 0.015, 0.85, 1.0)), glow);
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Lava lamp", makeGeneratedShader(R"GLSL(    vec2 q = p;
    float field = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        float fi = float(i);
        vec2 center = vec2(sin(uTime * (0.35 + fi * 0.07) + fi * 2.1) * 0.75, cos(uTime * (0.28 + fi * 0.05) + fi) * 0.32);
        field += 0.11 / max(dot(q - center, q - center), 0.02);
    }
    float blob = smoothstep(1.1, 2.4, field);
    vec3 color = mix(vec3(0.02, 0.0, 0.03), vec3(1.0, 0.16, 0.02), blob);
    color += vec3(0.8, 0.02, 0.35) * smoothstep(2.0, 4.5, field) * 0.6;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Ocean caustics", makeGeneratedShader(R"GLSL(    vec2 q = p * 3.0;
    float a = sin(q.x * 2.0 + uTime * 1.2) + sin(q.y * 3.1 - uTime * 0.8);
    float b = sin((q.x + q.y) * 2.7 + uTime * 0.6);
    float caustic = pow(0.5 + 0.5 * sin((a + b) * 2.3), 7.0);
    vec3 color = vec3(0.0, 0.08, 0.16) + vec3(0.05, 0.65, 1.0) * caustic;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Synthwave horizon", makeGeneratedShader(R"GLSL(    vec2 q = p;
    float horizon = exp(-abs(q.y + 0.03) * 16.0);
    float gridX = pow(1.0 - abs(fract((q.x / max(q.y + 0.65, 0.08)) * 3.0) - 0.5) * 2.0, 18.0);
    float gridY = pow(1.0 - abs(fract((1.0 / max(q.y + 0.72, 0.08) + uTime * 0.5) * 3.0) - 0.5) * 2.0, 18.0);
    float sun = smoothstep(0.34, 0.31, length(vec2(q.x, q.y - 0.18)));
    vec3 color = vec3(0.015, 0.0, 0.05);
    color += vec3(1.0, 0.05, 0.65) * horizon;
    color += vec3(0.05, 0.35, 1.0) * max(gridX, gridY) * step(q.y, 0.02) * 0.8;
    color += vec3(1.0, 0.2, 0.04) * sun * 0.75;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Matrix rain", makeGeneratedShader(R"GLSL(    float xCell = floor(uv.x * 32.0);
    float speed = 0.8 + hash11(xCell) * 2.2;
    float head = fract(hash11(xCell * 7.1) + uTime * speed * 0.16);
    float y = fract(uv.y + head);
    float trail = exp(-y * 8.0);
    float glyph = step(0.55, hash11(floor(uv.y * 22.0) + xCell * 31.0 + floor(uTime * 8.0)));
    vec3 color = vec3(0.0, 0.04, 0.01) + vec3(0.05, 1.0, 0.22) * trail * (0.25 + 0.75 * glyph);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Voronoi neon", makeGeneratedShader(R"GLSL(    vec2 q = uv * vec2(12.0, 5.0);
    vec2 cell = floor(q);
    vec2 f = fract(q);
    float nearest = 10.0;
    vec2 nearestId = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 id = cell + vec2(float(x), float(y));
            vec2 point = vec2(hash21(id), hash21(id + 17.3));
            point = 0.5 + 0.38 * sin(uTime * 0.7 + 6.2831 * point);
            float d = length(vec2(float(x), float(y)) + point - f);
            if (d < nearest) { nearest = d; nearestId = id; }
        }
    float edge = smoothstep(0.12, 0.02, nearest);
    vec3 base = hsv2rgb(vec3(fract(hash21(nearestId) + uTime * 0.01), 0.8, 0.35));
    vec3 color = base + vec3(0.25, 0.8, 1.0) * edge;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Polar spokes", makeGeneratedShader(R"GLSL(    float angle = atan(p.y, p.x);
    float radius = length(p);
    float spokes = pow(0.5 + 0.5 * cos(angle * 18.0 + uTime * 2.2), 8.0);
    float ring = pow(0.5 + 0.5 * cos(radius * 22.0 - uTime * 3.0), 10.0);
    vec3 color = hsv2rgb(vec3(fract(angle / 6.2831 + uTime * 0.03), 0.9, clamp(spokes * 0.8 + ring * 0.6, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Neon triangle", makeGeneratedShader(R"GLSL(    vec2 q = rotate2d(p, uTime * 0.5);
    float d = sdEquilateralTriangle(q, 0.58);
    float glow = exp(-abs(d) * 28.0) + exp(-abs(d + 0.12) * 35.0) * 0.5;
    vec3 color = hsv2rgb(vec3(fract(0.78 + d * 0.5 + uTime * 0.04), 0.9, clamp(glow, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Orbiting dots", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.003, 0.006, 0.018);
    for (int i = 0; i < 8; ++i)
    {
        float fi = float(i);
        float angle = uTime * (0.55 + fi * 0.025) + fi * 0.7854;
        vec2 center = vec2(cos(angle), sin(angle)) * vec2(0.82, 0.34);
        float glow = exp(-length(p - center) * 18.0);
        color += hsv2rgb(vec3(fract(fi / 8.0 + uTime * 0.02), 0.9, 1.0)) * glow;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Chromatic scanlines", makeGeneratedShader(R"GLSL(    float scan = pow(0.5 + 0.5 * sin((uv.y * 42.0 - uTime * 4.0) * 3.14159), 9.0);
    float sweep = 0.35 + 0.65 * pow(0.5 + 0.5 * sin(uv.x * 8.0 + uTime), 3.0);
    vec3 color = hsv2rgb(vec3(fract(uv.x * 0.7 + uv.y * 0.25 - uTime * 0.08), 1.0, scan * sweep));
    color += vec3(0.01, 0.01, 0.025);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Moiré waves", makeGeneratedShader(R"GLSL(    float a = sin(length(p - vec2(-0.55, 0.0)) * 34.0 - uTime * 2.0);
    float b = sin(length(p - vec2(0.55, 0.0)) * 31.0 + uTime * 1.7);
    float v = 0.5 + 0.5 * sin((a + b) * 4.0);
    vec3 color = hsv2rgb(vec3(fract(v * 0.22 + uTime * 0.02), 0.85, pow(v, 2.2)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Audio mirror bars", makeGeneratedShader(R"GLSL(    float band = clamp(uBands[column], 0.0, 1.0);
    float centerDistance = abs(uv.y - 0.5) * 2.0;
    float lit = 1.0 - smoothstep(band, band + 0.08, 1.0 - centerDistance);
    lit *= step(centerDistance, band);
    vec3 color = hsv2rgb(vec3(fract(float(column) / 16.0 - uTime * 0.05), 0.95, lit));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Audio equalizer dots", makeGeneratedShader(R"GLSL(    float band = clamp(uBands[column], 0.0, 1.0);
    float targetY = band;
    float dot = exp(-abs(uv.y - targetY) * 32.0);
    float trail = smoothstep(targetY, 0.0, uv.y) * 0.16;
    vec3 color = hsv2rgb(vec3(fract(float(column) / 20.0 + uTime * 0.03), 0.9, clamp(dot + trail, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Audio radial flower", makeGeneratedShader(R"GLSL(    float angle = atan(p.y, p.x);
    int bandIndex = clamp(int(floor((angle / 6.2831 + 0.5) * 16.0)), 0, 15);
    float radius = length(p);
    float target = 0.18 + uBands[bandIndex] * 0.75;
    float petal = exp(-abs(radius - target) * 18.0);
    vec3 color = hsv2rgb(vec3(fract(float(bandIndex) / 16.0 + uTime * 0.02), 0.9, petal));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Audio plasma bands", makeGeneratedShader(R"GLSL(    float energy = clamp(uBands[column], 0.0, 1.0);
    float wave = 0.5 + 0.5 * sin(p.x * 5.0 + p.y * 10.0 + uTime * 2.0 + energy * 8.0);
    vec3 color = hsv2rgb(vec3(fract(0.58 + energy * 0.35 + wave * 0.12), 0.9, (0.18 + energy * 0.82) * wave));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Audio tunnel", makeGeneratedShader(R"GLSL(    float radius = max(length(p), 0.02);
    float angle = atan(p.y, p.x);
    int bandIndex = clamp(int(fract(angle / 6.2831 + 0.5) * 16.0), 0, 15);
    float energy = uBands[bandIndex];
    float tunnel = pow(0.5 + 0.5 * sin(10.0 / radius - uTime * 4.0 + energy * 6.0), 5.0);
    vec3 color = hsv2rgb(vec3(fract(angle / 6.2831 + energy * 0.2), 0.9, tunnel * (0.25 + 0.75 * energy)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - comet trails", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.004, 0.01);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.2) continue;
        vec2 e = eventUv(event);
        vec2 head = e + vec2(age * 0.28, sin(age * 5.0 + event.x) * 0.04);
        float glow = exp(-length((uv - head) * vec2(2.0, 4.0)) * 17.0) * exp(-age * 0.75);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + age * 0.08), 0.9, 1.0)) * glow;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - diamond shockwave", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.003, 0.003, 0.012);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.0) continue;
        vec2 d = abs((uv - eventUv(event)) * vec2(1.0, 2.4));
        float dist = d.x + d.y;
        float ring = exp(-abs(dist - age * 0.8) * 24.0) * exp(-age * 0.9);
        color += vec3(0.18, 0.65, 1.0) * ring;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - confetti", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.003, 0.008);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.6) continue;
        for (int j = 0; j < 3; ++j)
        {
            float seed = event.x * 31.0 + event.y * 17.0 + float(j) * 11.0;
            float angle = hash11(seed) * 6.2831;
            vec2 particle = eventUv(event) + vec2(cos(angle), sin(angle)) * age * (0.12 + hash11(seed + 2.0) * 0.32);
            particle.y -= age * age * 0.08;
            float glow = exp(-length((uv - particle) * vec2(1.0, 2.4)) * 38.0) * exp(-age * 1.5);
            color += hsv2rgb(vec3(hash11(seed + 4.0), 0.95, 1.0)) * glow;
        }
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - color splash", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.004, 0.006, 0.012);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 3.0) continue;
        float dist = length((uv - eventUv(event)) * vec2(1.0, 2.3));
        float splash = exp(-dist * (5.0 + age * 2.5)) * exp(-age * 0.6);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + event.y * 0.08), 0.85, 1.0)) * splash;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - grid pulse", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.005, 0.009);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.8) continue;
        float dx = abs(float(column) - event.x);
        float dy = abs(float(row) - event.y);
        float linePulse = exp(-min(dx, dy) * 2.2) * exp(-abs(max(dx, dy) - age * 8.0) * 1.6) * exp(-age * 1.0);
        color += vec3(0.75, 0.18, 1.0) * linePulse;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - row chase", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.004, 0.009);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5 || int(event.y) != row) continue;
        float age = max(uTime - event.z, 0.0);
        float front = age * 12.0;
        float chase = exp(-abs(abs(float(column) - event.x) - front) * 1.6) * exp(-age * 0.8);
        color += hsv2rgb(vec3(fract(event.y / 6.0 + age * 0.07), 0.9, 1.0)) * chase;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - column chase", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.004, 0.009);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5 || int(event.x) != column) continue;
        float age = max(uTime - event.z, 0.0);
        float front = age * 5.0;
        float chase = exp(-abs(abs(float(row) - event.y) - front) * 2.2) * exp(-age * 0.9);
        color += vec3(0.05, 0.75, 1.0) * chase;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - echo rings", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.003, 0.008);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        vec2 d = (uv - eventUv(event)) * vec2(1.0, 2.3);
        float dist = length(d);
        float rings = 0.0;
        for (int r = 0; r < 3; ++r)
            rings += exp(-abs(dist - age * 0.55 + float(r) * 0.18) * 28.0) * exp(-age * (0.8 + float(r) * 0.15));
        color += vec3(0.1, 1.0, 0.72) * rings;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - lightning", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.002, 0.008);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 0.65) continue;
        vec2 e = eventUv(event);
        float jag = sin((uv.x - e.x) * 55.0 + event.x * 3.0) * 0.035 + sin((uv.x - e.x) * 91.0) * 0.014;
        float bolt = exp(-abs(uv.y - e.y - jag) * 80.0) * exp(-abs(uv.x - e.x) * 2.0) * exp(-age * 5.0);
        color += vec3(0.55, 0.75, 1.0) * bolt;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - bloom", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.003, 0.007);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.4) continue;
        vec2 d = (uv - eventUv(event)) * vec2(1.0, 2.4);
        float angle = atan(d.y, d.x);
        float radius = length(d);
        float petals = pow(abs(cos(angle * 5.0)), 4.0);
        float bloom = exp(-abs(radius - age * 0.22 * (0.6 + petals)) * 22.0) * exp(-age * 0.8);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + 0.82), 0.72, 1.0)) * bloom;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - embers", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.01, 0.002, 0.0);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.8) continue;
        float seed = event.x * 23.0 + event.y * 47.0;
        vec2 particle = eventUv(event) + vec2(sin(seed + age * 4.0) * 0.08, age * 0.24);
        float ember = exp(-length((uv - particle) * vec2(1.2, 2.4)) * 35.0) * exp(-age * 0.9);
        color += mix(vec3(1.0, 0.06, 0.0), vec3(1.0, 0.75, 0.05), exp(-age * 2.0)) * ember;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - constellation", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.004, 0.012);
    for (int i = 0; i < 16; ++i)
    {
        vec4 a = uKeyEvents[i];
        if (a.w < 0.5) continue;
        float ageA = max(uTime - a.z, 0.0);
        if (ageA > 3.0) continue;
        vec2 pa = eventUv(a);
        float star = exp(-length((uv - pa) * vec2(1.0, 2.4)) * 42.0) * exp(-ageA * 0.7);
        color += vec3(0.45, 0.75, 1.0) * star;
        for (int j = i + 1; j < 16; ++j)
        {
            vec4 b = uKeyEvents[j];
            if (b.w < 0.5 || max(uTime - b.z, 0.0) > 3.0) continue;
            vec2 pb = eventUv(b);
            vec2 ab = pb - pa;
            float t = clamp(dot(uv - pa, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0);
            float line = exp(-length((uv - (pa + ab * t)) * vec2(1.0, 2.4)) * 75.0) * exp(-ageA * 0.8);
            color += vec3(0.08, 0.22, 0.55) * line;
        }
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - key paint", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.003, 0.004, 0.009);
    float held = uKeyState[row * 16 + column];
    if (held > 0.5)
        color = hsv2rgb(vec3(fract(float(column) / 16.0 + float(row) * 0.07 + uTime * 0.05), 0.9, 1.0));
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5 || int(event.x) != column || int(event.y) != row) continue;
        float age = max(uTime - event.z, 0.0);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + event.y * 0.08), 0.8, 1.0)) * exp(-age * 0.45);
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - sonar sweep", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.006, 0.006);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.4) continue;
        vec2 d = (uv - eventUv(event)) * vec2(1.0, 2.3);
        float angle = atan(d.y, d.x);
        float sweepAngle = age * 5.5;
        float delta = abs(atan(sin(angle - sweepAngle), cos(angle - sweepAngle)));
        float beam = exp(-delta * 12.0) * exp(-length(d) * 1.4) * exp(-age * 0.55);
        color += vec3(0.05, 1.0, 0.55) * beam;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - pixel explosion", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.003, 0.008);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 1.8) continue;
        float dx = abs(float(column) - event.x);
        float dy = abs(float(row) - event.y);
        float shell = exp(-abs(max(dx / 2.0, dy) - age * 5.0) * 2.5) * exp(-age * 0.95);
        color += hsv2rgb(vec3(fract(event.x / 16.0 + age * 0.15), 0.9, 1.0)) * shell;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - magnetic field", makeGeneratedShader(R"GLSL(    vec2 field = vec2(0.0);
    float strength = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 3.0) continue;
        vec2 d = (uv - eventUv(event)) * vec2(1.0, 2.3);
        float inv = exp(-age * 0.7) / max(dot(d, d), 0.01);
        field += vec2(-d.y, d.x) * inv;
        strength += inv * 0.04;
    }
    float lines = pow(0.5 + 0.5 * sin(atan(field.y, field.x) * 12.0 + length(field) * 0.08), 7.0);
    vec3 color = hsv2rgb(vec3(fract(length(field) * 0.01 + 0.55), 0.9, clamp(lines * strength, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - falling rain", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.001, 0.004, 0.01);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.2) continue;
        vec2 e = eventUv(event);
        vec2 drop = e + vec2(sin(event.x * 4.0 + age * 2.0) * 0.02, -age * 0.35);
        float streak = exp(-abs(uv.x - drop.x) * 90.0) * exp(-abs(uv.y - drop.y) * 18.0) * exp(-age * 0.7);
        color += vec3(0.1, 0.55, 1.0) * streak;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - rainbow bloom", makeGeneratedShader(R"GLSL(    vec3 color = vec3(0.002, 0.003, 0.008);
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        if (age > 2.6) continue;
        float dist = length((uv - eventUv(event)) * vec2(1.0, 2.3));
        float wave = exp(-abs(dist - age * 0.32) * 19.0) * exp(-age * 0.65);
        color += hsv2rgb(vec3(fract(dist * 0.75 - age * 0.2 + event.x / 16.0), 0.95, 1.0)) * wave;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Reactive - held gradient", makeGeneratedShader(R"GLSL(    float held = uKeyState[row * 16 + column];
    vec3 color = hsv2rgb(vec3(fract(uv.x * 0.6 + uv.y * 0.25 - uTime * 0.035), 0.85, 0.06));
    if (held > 0.5)
        color = hsv2rgb(vec3(fract(uv.x + uTime * 0.08), 0.95, 1.0));
    for (int i = 0; i < 16; ++i)
    {
        vec4 event = uKeyEvents[i];
        if (event.w < 0.5) continue;
        float age = max(uTime - event.z, 0.0);
        float dist = length((uv - eventUv(event)) * vec2(1.0, 2.3));
        color += hsv2rgb(vec3(fract(event.x / 16.0 + age * 0.04), 0.8, 1.0)) * exp(-dist * 10.0) * exp(-age * 1.4) * 0.5;
    }
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - health bar", makeSourceShader(
            "uniform float uHealth; // @ui min=0.0 max=1.0 step=0.001 default=1.0 label=\"Health\" id=source.health",
            R"GLSL(    float health = clamp(uHealth, 0.0, 1.0);
    float filled = step(uv.x, health);
    vec3 low = vec3(1.0, 0.02, 0.01);
    vec3 high = vec3(0.05, 1.0, 0.22);
    vec3 color = mix(low, high, smoothstep(0.0, 1.0, health)) * filled;
    color += vec3(0.025) * (1.0 - filled);
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Data - process reactor", makeSourceShader(
            "uniform float uProcessValue; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label=\"Process value\" id=process.value",
            R"GLSL(    float value = clamp(uProcessValue, 0.0, 1.0);
    float wave = 0.5 + 0.5 * sin((p.x * 8.0 + p.y * 4.0) - uTime * (1.0 + value * 8.0));
    float core = exp(-length(p) * (2.0 + value * 4.0));
    vec3 color = hsv2rgb(vec3(fract(0.58 + value * 0.35 + wave * 0.08), 0.95, clamp(core + wave * value, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Data - binding status", makeSourceShader(
            "uniform float uBindingStatus; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label=\"Binding status\" id=binding.status",
            R"GLSL(    float value = clamp(uBindingStatus, 0.0, 1.0);
    float pulse = 0.65 + 0.35 * sin(uTime * (1.5 + value * 5.0) - length(p) * 10.0);
    vec3 offline = vec3(0.10, 0.01, 0.015);
    vec3 online = vec3(0.05, 0.85, 1.0);
    vec3 color = mix(offline, online, value) * (0.20 + pulse * (0.35 + value * 0.65));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Data - telemetry gauge", makeSourceShader(
            "uniform float uTelemetryValue; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label=\"Telemetry\" id=telemetry.value",
            R"GLSL(    float value = clamp(uTelemetryValue, 0.0, 1.0);
    float meter = step(uv.y, value);
    vec3 color = hsv2rgb(vec3(0.34 - value * 0.34, 0.95, 0.12 + 0.88 * meter));
    color *= 0.7 + 0.3 * sin(float(column) * 0.9 + uTime * 3.0 * value);
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Data - external audio pulse", makeSourceShader(
            "uniform float uExternalAudio; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label=\"External audio\" id=audio.external",
            R"GLSL(    float level = clamp(uExternalAudio, 0.0, 1.0);
    float radius = length(p);
    float ring = exp(-pow((radius - 0.15 - level * 0.7) * 12.0, 2.0));
    vec3 color = hsv2rgb(vec3(fract(uv.x + uTime * 0.05), 0.9, clamp(level * 0.55 + ring, 0.0, 1.0)));
    FragColor = vec4(applyIndicators(color, row, column), 1.0);)GLSL")});
        presets.push_back({"Data - runtime dashboard", makeSourceShader(
            R"GLSL(uniform float uNative; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label="Native process" id=runtime.native
uniform float uBindingStatus; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label="Binding status" id=runtime.status
uniform float uAudioData; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label="Audio" id=runtime.audio
uniform float uSystemData; // @ui min=0.0 max=1.0 step=0.001 default=0.0 label="Telemetry" id=runtime.system)GLSL",
            R"GLSL(    float nativeValue = clamp(uNative, 0.0, 1.0);
    float statusValue = clamp(uBindingStatus, 0.0, 1.0);
    float audioValue = clamp(uAudioData, 0.0, 1.0);
    float systemValue = clamp(uSystemData, 0.0, 1.0);
    float quadrant = step(0.5, uv.x) + step(0.5, uv.y) * 2.0;
    float value = quadrant < 0.5 ? nativeValue : quadrant < 1.5 ? statusValue : quadrant < 2.5 ? audioValue : systemValue;
    vec3 baseColor = quadrant < 0.5 ? vec3(0.08, 0.75, 1.0) : quadrant < 1.5 ? vec3(0.10, 0.75, 1.0) : quadrant < 2.5 ? vec3(0.10, 1.0, 0.35) : vec3(1.0, 0.28, 0.06);
    float scan = 0.65 + 0.35 * sin((uv.x + uv.y) * 16.0 - uTime * (1.0 + value * 5.0));
    float separator = smoothstep(0.02, 0.0, min(abs(uv.x - 0.5), abs(uv.y - 0.5)));
    vec3 color = baseColor * (0.05 + value * scan) + vec3(separator * 0.18);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - health tri-color", makeSourceShader(
            R"GLSL(uniform float uHealthValue; // @ui min=0.0 max=10000.0 step=1.0 default=100.0 label="Health" id=health.value
uniform float uHealthMaximum; // @ui min=0.001 max=10000.0 step=1.0 default=100.0 label="Maximum health" id=health.maximum
uniform vec3 uHealthHighColor; // @ui color default=0.05,1.0,0.12 label="Healthy color" id=health.color.high
uniform vec3 uHealthMidColor; // @ui color default=1.0,0.72,0.02 label="Half health color" id=health.color.mid
uniform vec3 uHealthLowColor; // @ui color default=1.0,0.02,0.01 label="Critical color" id=health.color.low
uniform float uHealthCriticalPulse; // @ui min=0.0 max=2.0 step=0.01 default=0.65 label="Critical pulse" id=health.critical_pulse)GLSL",
            R"GLSL(    float health = clamp(uHealthValue / max(uHealthMaximum, 0.001), 0.0, 1.0);
    vec3 color = health < 0.5
        ? mix(uHealthLowColor, uHealthMidColor, smoothstep(0.0, 0.5, health))
        : mix(uHealthMidColor, uHealthHighColor, smoothstep(0.5, 1.0, health));
    float critical = 1.0 - smoothstep(0.08, 0.25, health);
    float pulse = mix(1.0, 0.55 + 0.45 * sin(uTime * 8.0), critical * uHealthCriticalPulse);
    float fill = mix(0.28, 1.0, step(uv.x, health));
    color *= pulse * fill;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - threshold bands", makeSourceShader(
            R"GLSL(uniform float uRuntimeValue; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Value" id=runtime.value
uniform float uThresholdLow; // @ui min=0.0 max=1.0 step=0.001 default=0.35 label="Low threshold" id=runtime.threshold.low
uniform float uThresholdHigh; // @ui min=0.0 max=1.0 step=0.001 default=0.70 label="High threshold" id=runtime.threshold.high
uniform vec3 uThresholdLowColor; // @ui color default=0.05,1.0,0.18 label="Low band" id=runtime.color.low
uniform vec3 uThresholdMidColor; // @ui color default=1.0,0.75,0.02 label="Middle band" id=runtime.color.mid
uniform vec3 uThresholdHighColor; // @ui color default=1.0,0.03,0.02 label="High band" id=runtime.color.high)GLSL",
            R"GLSL(    float value = clamp(uRuntimeValue, 0.0, 1.0);
    float low = min(uThresholdLow, uThresholdHigh);
    float high = max(uThresholdLow, uThresholdHigh);
    vec3 color = value < low ? uThresholdLowColor : value < high ? uThresholdMidColor : uThresholdHighColor;
    float marker = exp(-abs(uv.x - value) * 70.0);
    float band = 0.35 + 0.65 * step(uv.x, value);
    color *= band + marker * 0.4;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - dual meter", makeSourceShader(
            R"GLSL(uniform float uRuntimePrimary; // @ui min=0.0 max=1.0 step=0.001 default=0.7 label="Primary" id=runtime.primary
uniform float uRuntimeSecondary; // @ui min=0.0 max=1.0 step=0.001 default=0.35 label="Secondary" id=runtime.secondary
uniform vec3 uPrimaryColor; // @ui color default=0.05,0.65,1.0 label="Primary color" id=runtime.primary.color
uniform vec3 uSecondaryColor; // @ui color default=1.0,0.15,0.65 label="Secondary color" id=runtime.secondary.color)GLSL",
            R"GLSL(    float a = clamp(uRuntimePrimary, 0.0, 1.0);
    float b = clamp(uRuntimeSecondary, 0.0, 1.0);
    bool upper = uv.y >= 0.5;
    float value = upper ? a : b;
    vec3 base = upper ? uPrimaryColor : uSecondaryColor;
    float fill = step(uv.x, value);
    float edge = exp(-abs(uv.x - value) * 55.0);
    vec3 color = base * (0.08 + fill * 0.82 + edge * 0.45);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - telemetry lanes", makeSourceShader(
            R"GLSL(uniform float uRuntimeA; // @ui min=0.0 max=1.0 step=0.001 default=0.2 label="Lane A" id=runtime.a
uniform float uRuntimeB; // @ui min=0.0 max=1.0 step=0.001 default=0.4 label="Lane B" id=runtime.b
uniform float uRuntimeC; // @ui min=0.0 max=1.0 step=0.001 default=0.6 label="Lane C" id=runtime.c
uniform float uRuntimeD; // @ui min=0.0 max=1.0 step=0.001 default=0.8 label="Lane D" id=runtime.d)GLSL",
            R"GLSL(    float lane = floor(uv.x * 4.0);
    float value = lane < 0.5 ? uRuntimeA : lane < 1.5 ? uRuntimeB : lane < 2.5 ? uRuntimeC : uRuntimeD;
    value = clamp(value, 0.0, 1.0);
    vec3 laneColor = lane < 0.5 ? vec3(0.05,0.7,1.0) : lane < 1.5 ? vec3(0.45,0.2,1.0) : lane < 2.5 ? vec3(0.1,1.0,0.4) : vec3(1.0,0.35,0.04);
    float level = step(uv.y, value);
    float scan = 0.72 + 0.28 * sin(uTime * (1.5 + value * 5.0) + uv.y * 18.0);
    vec3 color = laneColor * (0.035 + level * scan);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - balance scanner", makeSourceShader(
            R"GLSL(uniform float uRuntimeLeft; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Left" id=runtime.left
uniform float uRuntimeRight; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Right" id=runtime.right
uniform vec3 uBalanceLeftColor; // @ui color default=0.05,0.55,1.0 label="Left color" id=runtime.left.color
uniform vec3 uBalanceRightColor; // @ui color default=1.0,0.12,0.45 label="Right color" id=runtime.right.color)GLSL",
            R"GLSL(    float left = max(uRuntimeLeft, 0.0);
    float right = max(uRuntimeRight, 0.0);
    float total = max(left + right, 0.0001);
    float balance = clamp(right / total, 0.0, 1.0);
    float energy = clamp(total * 0.5, 0.0, 1.0);
    float scanner = exp(-abs(uv.x - balance) * 48.0);
    vec3 gradient = mix(uBalanceLeftColor, uBalanceRightColor, uv.x);
    vec3 color = gradient * (0.03 + energy * 0.28 + scanner * (0.6 + 0.4 * energy));
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - runtime wave", makeSourceShader(
            R"GLSL(uniform float uRuntimeWaveValue; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Wave value" id=runtime.wave.value
uniform float uRuntimeWaveDrive; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Drive" id=runtime.wave.drive
uniform vec3 uRuntimeWaveColor; // @ui color default=0.1,0.75,1.0 label="Wave color" id=runtime.wave.color)GLSL",
            R"GLSL(    float value = clamp(uRuntimeWaveValue, 0.0, 1.0);
    float drive = clamp(uRuntimeWaveDrive, 0.0, 1.0);
    float center = 0.5 + sin(uv.x * (7.0 + drive * 16.0) - uTime * (1.0 + drive * 8.0)) * (0.05 + value * 0.35);
    float line = exp(-abs(uv.y - center) * (26.0 + drive * 34.0));
    float glow = exp(-abs(uv.y - center) * 7.0);
    vec3 color = uRuntimeWaveColor * (line + glow * (0.05 + value * 0.35));
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - segmented gauge", makeSourceShader(
            R"GLSL(uniform float uGaugeValue; // @ui min=0.0 max=1.0 step=0.001 default=0.65 label="Gauge" id=runtime.gauge
uniform vec3 uGaugeHighColor; // @ui color default=0.05,1.0,0.16 label="High color" id=runtime.gauge.high
uniform vec3 uGaugeMidColor; // @ui color default=1.0,0.72,0.02 label="Middle color" id=runtime.gauge.mid
uniform vec3 uGaugeLowColor; // @ui color default=1.0,0.03,0.02 label="Low color" id=runtime.gauge.low)GLSL",
            R"GLSL(    float value = clamp(uGaugeValue, 0.0, 1.0);
    float segment = (float(column) + 1.0) / 16.0;
    float lit = step(segment, value + 0.0001);
    vec3 tone = value < 0.5 ? mix(uGaugeLowColor, uGaugeMidColor, value * 2.0) : mix(uGaugeMidColor, uGaugeHighColor, (value - 0.5) * 2.0);
    float rowPulse = 0.82 + 0.18 * sin(uTime * 2.5 + float(row) * 0.7);
    vec3 color = tone * (0.025 + lit * rowPulse);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - source crossfade", makeSourceShader(
            R"GLSL(uniform float uRuntimeMix; // @ui min=0.0 max=1.0 step=0.001 default=0.5 label="Crossfade" id=runtime.mix
uniform float uRuntimeIntensity; // @ui min=0.0 max=1.0 step=0.001 default=1.0 label="Intensity" id=runtime.intensity
uniform vec3 uRuntimeColorA; // @ui color default=0.03,0.45,1.0 label="Color A" id=runtime.mix.color_a
uniform vec3 uRuntimeColorB; // @ui color default=1.0,0.08,0.55 label="Color B" id=runtime.mix.color_b)GLSL",
            R"GLSL(    float blend = clamp(uRuntimeMix, 0.0, 1.0);
    float intensity = clamp(uRuntimeIntensity, 0.0, 1.0);
    float spatial = smoothstep(blend - 0.18, blend + 0.18, uv.x);
    vec3 color = mix(uRuntimeColorA, uRuntimeColorB, spatial);
    float edge = exp(-abs(uv.x - blend) * 32.0);
    color *= intensity * (0.35 + 0.65 * (0.7 + 0.3 * sin(uTime * 2.0 + uv.y * 8.0))) + edge * 0.25;
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - alert beacon", makeSourceShader(
            R"GLSL(uniform float uAlertValue; // @ui min=0.0 max=1.0 step=0.001 default=0.2 label="Alert value" id=runtime.alert
uniform float uAlertWarning; // @ui min=0.0 max=1.0 step=0.001 default=0.55 label="Warning threshold" id=runtime.alert.warning
uniform float uAlertCritical; // @ui min=0.0 max=1.0 step=0.001 default=0.82 label="Critical threshold" id=runtime.alert.critical
uniform vec3 uAlertSafeColor; // @ui color default=0.05,0.9,0.22 label="Safe color" id=runtime.alert.safe_color
uniform vec3 uAlertWarningColor; // @ui color default=1.0,0.68,0.02 label="Warning color" id=runtime.alert.warning_color
uniform vec3 uAlertCriticalColor; // @ui color default=1.0,0.02,0.01 label="Critical color" id=runtime.alert.critical_color)GLSL",
            R"GLSL(    float value = clamp(uAlertValue, 0.0, 1.0);
    float warning = min(uAlertWarning, uAlertCritical);
    float critical = max(uAlertWarning, uAlertCritical);
    vec3 color = value < warning ? uAlertSafeColor : value < critical ? uAlertWarningColor : uAlertCriticalColor;
    float severity = value < warning ? 0.0 : value < critical ? 0.5 : 1.0;
    float flash = severity <= 0.0 ? 1.0 : 0.48 + 0.52 * sin(uTime * mix(4.0, 10.0, severity));
    float sweep = 0.72 + 0.28 * sin((uv.x + uv.y) * 15.0 - uTime * (2.0 + severity * 5.0));
    color *= mix(0.45 + value * 0.55, flash * sweep, severity);
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        presets.push_back({"Data - four-corner field", makeSourceShader(
            R"GLSL(uniform float uCornerA; // @ui min=0.0 max=1.0 step=0.001 default=0.15 label="Top left" id=runtime.corner.a
uniform float uCornerB; // @ui min=0.0 max=1.0 step=0.001 default=0.40 label="Top right" id=runtime.corner.b
uniform float uCornerC; // @ui min=0.0 max=1.0 step=0.001 default=0.65 label="Bottom left" id=runtime.corner.c
uniform float uCornerD; // @ui min=0.0 max=1.0 step=0.001 default=0.90 label="Bottom right" id=runtime.corner.d)GLSL",
            R"GLSL(    float top = mix(clamp(uCornerA,0.0,1.0), clamp(uCornerB,0.0,1.0), uv.x);
    float bottom = mix(clamp(uCornerC,0.0,1.0), clamp(uCornerD,0.0,1.0), uv.x);
    float value = mix(bottom, top, uv.y);
    float contour = 0.65 + 0.35 * sin(value * 28.0 - uTime * (1.0 + value * 4.0));
    vec3 color = hsv2rgb(vec3(mix(0.66, 0.0, value), 0.95, 0.08 + value * contour));
    FragColor = vec4(applyIndicators(clamp(color, 0.0, 1.0), row, column), 1.0);)GLSL")});
        for (auto& preset : presets) parameterizeShaderPreset(preset);
        return presets;
    }

    static const std::vector<ShaderPreset> ShaderPresets = buildShaderPresets();

    static_assert(sizeof(Color32) == 3);
    static_assert(sizeof(FramebufferSetPayload<MatrixSize>) == 340);
    static_assert(sizeof(PerformancePayload) == 36);
    static_assert(sizeof(MatrixTimingProbeResult<ActiveProbeRows>) == 52);

    struct PerformanceSnapshot
    {
        std::uint32_t CoreClock = 0;
        std::uint32_t BeginScanTicks = 0;
        std::uint32_t ScanTicks = 0;
        std::uint32_t EndScanTicks = 0;
        std::uint32_t StateUpdateTicks = 0;
        std::uint32_t HIDTicks = 0;
        std::uint32_t RGBTicks = 0;
        std::uint32_t AverageScanPeriodTicks = 0;
        std::uint32_t RGBSlotMaxTicks = 0;
    };

    struct SharedDeviceState
    {
        std::mutex Mutex;
        PerformanceSnapshot Performance{};
        MatrixTimingProbeResult<ActiveProbeRows> TimingProbe{};
        bool HasPerformance = false;
        bool HasTimingProbe = false;
        std::uint64_t ReceivedPackets = 0;
    };


    struct ReactiveKeyBinding
    {
        std::uint16_t Key;
        std::uint8_t Row;
        std::uint8_t Column;
    };

    constexpr auto ReactiveKeyBindings = std::to_array<ReactiveKeyBinding>({
        {KEY_ESC, 0, 0}, {KEY_F1, 0, 1}, {KEY_F2, 0, 2}, {KEY_F3, 0, 3}, {KEY_F4, 0, 4}, {KEY_F5, 0, 5}, {KEY_F6, 0, 6}, {KEY_F7, 0, 7},
        {KEY_F8, 0, 8}, {KEY_F9, 0, 9}, {KEY_F10, 0, 10}, {KEY_F11, 0, 11}, {KEY_F12, 0, 12}, {KEY_SYSRQ, 0, 13}, {KEY_SCROLLLOCK, 0, 14}, {KEY_PAUSE, 0, 15},
        {KEY_GRAVE, 1, 0}, {KEY_1, 1, 1}, {KEY_2, 1, 2}, {KEY_3, 1, 3}, {KEY_4, 1, 4}, {KEY_5, 1, 5}, {KEY_6, 1, 6}, {KEY_7, 1, 7},
        {KEY_8, 1, 8}, {KEY_9, 1, 9}, {KEY_0, 1, 10}, {KEY_MINUS, 1, 11}, {KEY_EQUAL, 1, 12}, {KEY_BACKSPACE, 1, 12}, {KEY_INSERT, 1, 13}, {KEY_HOME, 1, 14}, {KEY_PAGEUP, 1, 15},
        {KEY_TAB, 2, 0}, {KEY_Q, 2, 1}, {KEY_W, 2, 2}, {KEY_E, 2, 3}, {KEY_R, 2, 4}, {KEY_T, 2, 5}, {KEY_Y, 2, 6}, {KEY_U, 2, 7},
        {KEY_I, 2, 8}, {KEY_O, 2, 9}, {KEY_P, 2, 10}, {KEY_LEFTBRACE, 2, 11}, {KEY_RIGHTBRACE, 2, 12}, {KEY_DELETE, 2, 13}, {KEY_END, 2, 14}, {KEY_PAGEDOWN, 2, 15},
        {KEY_CAPSLOCK, 3, 0}, {KEY_A, 3, 1}, {KEY_S, 3, 2}, {KEY_D, 3, 3}, {KEY_F, 3, 4}, {KEY_G, 3, 5}, {KEY_H, 3, 6}, {KEY_J, 3, 7},
        {KEY_K, 3, 8}, {KEY_L, 3, 9}, {KEY_SEMICOLON, 3, 10}, {KEY_APOSTROPHE, 3, 11}, {KEY_BACKSLASH, 3, 12}, {KEY_ENTER, 3, 15},
        {KEY_LEFTSHIFT, 4, 0}, {KEY_102ND, 4, 1}, {KEY_Z, 4, 2}, {KEY_X, 4, 3}, {KEY_C, 4, 4}, {KEY_V, 4, 5}, {KEY_B, 4, 6}, {KEY_N, 4, 7}, {KEY_M, 4, 8},
        {KEY_COMMA, 4, 9}, {KEY_DOT, 4, 10}, {KEY_SLASH, 4, 11}, {KEY_RO, 4, 12}, {KEY_UP, 4, 14}, {KEY_RIGHTSHIFT, 4, 15},
        {KEY_LEFTCTRL, 5, 0}, {KEY_LEFTMETA, 5, 1}, {KEY_LEFTALT, 5, 2}, {KEY_SPACE, 5, 6}, {KEY_RIGHTALT, 5, 9}, {KEY_MENU, 5, 11}, {KEY_RIGHTCTRL, 5, 12},
        {KEY_LEFT, 5, 13}, {KEY_DOWN, 5, 14}, {KEY_RIGHT, 5, 15}
    });

    static const ReactiveKeyBinding* findReactiveKeyBinding(const std::uint16_t key) noexcept
    {
        for (const auto& binding : ReactiveKeyBindings)
            if (binding.Key == key)
                return &binding;
        return nullptr;
    }

    struct ReactiveKeyEvent
    {
        float Column = 0.0f;
        float Row = 0.0f;
        float Time = 0.0f;
        float Valid = 0.0f;
    };

    struct ReactiveKeyState
    {
        static constexpr std::size_t EventCount = 16;
        std::array<float, MatrixSize> Down{};
        std::array<ReactiveKeyEvent, EventCount> Events{};
        std::size_t NextEvent = 0;
        bool CapsLockActive = false;
        bool ScrollLockActive = false;
    };

    class EvdevKeyboard
    {
    public:
        ~EvdevKeyboard() { stop(); }

        void start(const double glfwTime)
        {
            if (_running.exchange(true))
                return;
            _glfwBaseTime = glfwTime;
            _steadyBaseTime = std::chrono::steady_clock::now();
            _thread = std::thread(&EvdevKeyboard::run, this);
        }

        void stop() noexcept
        {
            if (!_running.exchange(false))
                return;
            if (_thread.joinable())
                _thread.join();
            closeDevices();
        }

        ReactiveKeyState snapshot() const
        {
            std::lock_guard lock(_mutex);
            return _state;
        }

        bool connected() const
        {
            std::lock_guard lock(_mutex);
            return _connected;
        }

        std::string deviceName() const
        {
            std::lock_guard lock(_mutex);
            return _deviceName;
        }

        std::string status() const
        {
            std::lock_guard lock(_mutex);
            return _status;
        }

        bool consumeRestoreRequest() noexcept { return _restoreRequested.exchange(false); }

    private:
        struct Device
        {
            int Fd = -1;
            std::string Path;
            std::string Name;
            bool HasKeys = false;
            bool HasCapsLed = false;
            bool HasScrollLed = false;
        };

        static constexpr std::size_t BitsPerLong = sizeof(unsigned long) * 8;

        static bool testBit(const unsigned long* bits, const std::size_t bit) noexcept
        {
            return (bits[bit / BitsPerLong] & (1UL << (bit % BitsPerLong))) != 0;
        }

        float now() const noexcept
        {
            return static_cast<float>(_glfwBaseTime + std::chrono::duration<double>(std::chrono::steady_clock::now() - _steadyBaseTime).count());
        }

        void closeDevices() noexcept
        {
            for (auto& device : _devices)
                if (device.Fd >= 0)
                    ::close(device.Fd);
            _devices.clear();
        }

        void resetKeyState()
        {
            _keyDown.fill(false);
            std::lock_guard lock(_mutex);
            _state.Down.fill(0.0f);
        }

        bool scanDevices()
        {
            closeDevices();
            bool permissionDenied = false;
            std::string firstName;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error))
            {
                if (error)
                    break;
                const std::string filename = entry.path().filename().string();
                if (!filename.starts_with("event"))
                    continue;
                const int fd = ::open(entry.path().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0)
                {
                    if (errno == EACCES || errno == EPERM)
                        permissionDenied = true;
                    continue;
                }
                input_id id{};
                if (::ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != VendorId || id.product != ProductId)
                {
                    ::close(fd);
                    continue;
                }
                char name[256]{};
                if (::ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
                    std::snprintf(name, sizeof(name), "Quartz keyboard");

                constexpr std::size_t EventWords = (EV_MAX + BitsPerLong) / BitsPerLong;
                constexpr std::size_t LedWords = (LED_MAX + BitsPerLong) / BitsPerLong;
                std::array<unsigned long, EventWords> eventBits{};
                std::array<unsigned long, LedWords> ledBits{};
                const bool gotEvents = ::ioctl(fd, EVIOCGBIT(0, sizeof(eventBits)), eventBits.data()) >= 0;
                const bool hasKeys = gotEvents && testBit(eventBits.data(), EV_KEY);
                const bool hasLedEvents = gotEvents && testBit(eventBits.data(), EV_LED);
                if (hasLedEvents)
                    ::ioctl(fd, EVIOCGBIT(EV_LED, sizeof(ledBits)), ledBits.data());
                const bool hasCapsLed = hasLedEvents && testBit(ledBits.data(), LED_CAPSL);
                const bool hasScrollLed = hasLedEvents && testBit(ledBits.data(), LED_SCROLLL);

                if (firstName.empty() && hasKeys)
                    firstName = name;
                _devices.push_back({fd, entry.path().string(), name, hasKeys, hasCapsLed, hasScrollLed});
            }

            _hasCapsLedNode = false;
            _hasScrollLedNode = false;
            _scrollLedInitialKnown = false;
            _scrollLedAuthoritative = false;
            std::size_t keyNodes = 0;
            std::size_t ledNodes = 0;
            for (const auto& device : _devices)
            {
                keyNodes += device.HasKeys ? 1u : 0u;
                if (device.HasCapsLed || device.HasScrollLed) ++ledNodes;
                _hasCapsLedNode |= device.HasCapsLed;
                _hasScrollLedNode |= device.HasScrollLed;
                if (firstName.empty()) firstName = device.Name;
            }
            resetKeyState();
            {
                std::lock_guard lock(_mutex);
                _connected = !_devices.empty();
                _deviceName = firstName;
                if (_connected)
                    _status = "evdev connected (" + std::to_string(keyNodes) + " key / " + std::to_string(ledNodes) + " LED nodes; Ctrl+Alt+Shift+Q restores window)";
                else if (permissionDenied)
                    _status = "No readable matching /dev/input/event* device; check evdev permissions/udev rules";
                else
                    _status = "Waiting for matching evdev keyboard";
            }
            if (!_devices.empty())
                refreshLedState();
            return !_devices.empty();
        }

        void refreshLedState()
        {
            constexpr std::size_t LedWords = (LED_MAX + BitsPerLong) / BitsPerLong;
            std::array<unsigned long, LedWords> leds{};
            bool capsKnown = false;
            bool scrollKnown = false;
            bool caps = false;
            bool scroll = false;
            for (const auto& device : _devices)
            {
                if (!device.HasCapsLed && !device.HasScrollLed)
                    continue;
                leds.fill(0);
                if (::ioctl(device.Fd, EVIOCGLED(sizeof(leds)), leds.data()) < 0)
                    continue;
                if (device.HasCapsLed)
                {
                    capsKnown = true;
                    caps |= testBit(leds.data(), LED_CAPSL);
                }
                if (device.HasScrollLed)
                {
                    scrollKnown = true;
                    scroll |= testBit(leds.data(), LED_SCROLLL);
                }
            }
            std::lock_guard lock(_mutex);
            if (capsKnown) _state.CapsLockActive = caps;
            if (scrollKnown)
            {
                _state.ScrollLockActive = scroll;
                _scrollLedInitialKnown = true;
                _initialScrollLedState = scroll;
                _scrollLedAuthoritative = false;
            }
        }

        bool restoreShortcutDown() const noexcept
        {
            const bool ctrl = _keyDown[KEY_LEFTCTRL] || _keyDown[KEY_RIGHTCTRL];
            const bool alt = _keyDown[KEY_LEFTALT] || _keyDown[KEY_RIGHTALT];
            const bool shift = _keyDown[KEY_LEFTSHIFT] || _keyDown[KEY_RIGHTSHIFT];
            return ctrl && alt && shift && _keyDown[KEY_Q];
        }

        void handleKey(const input_event& event)
        {
            if (event.code > KEY_MAX) return;
            const bool wasDown = _keyDown[event.code];
            _keyDown[event.code] = event.value != 0;
            if (event.value == 1 && !wasDown && restoreShortcutDown()) _restoreRequested.store(true);

            const auto* binding = findReactiveKeyBinding(event.code);
            if (!binding) return;
            const std::size_t index = static_cast<std::size_t>(binding->Row) * Columns + binding->Column;
            std::lock_guard lock(_mutex);
            _state.Down[index] = event.value == 0 ? 0.0f : 1.0f;
            if (event.value == 1 && !wasDown)
            {
                _state.Events[_state.NextEvent] = {static_cast<float>(binding->Column), static_cast<float>(binding->Row), now(), 1.0f};
                _state.NextEvent = (_state.NextEvent + 1) % ReactiveKeyState::EventCount;
                if (event.code == KEY_CAPSLOCK) _state.CapsLockActive = !_state.CapsLockActive;
                if (event.code == KEY_SCROLLLOCK) _state.ScrollLockActive = !_state.ScrollLockActive;
            }
        }

        void handleLed(const input_event& event)
        {
            if (event.code != LED_CAPSL && event.code != LED_SCROLLL)
                return;
            std::lock_guard lock(_mutex);
            if (event.code == LED_CAPSL)
            {
                _state.CapsLockActive = event.value != 0;
                return;
            }

            // Scroll Lock is frequently advertised through EV_LED even when the desktop/XKB
            // never actually toggles it. Keep the physical-key fallback unless we observe the
            // LED leave its startup state; once that happens, the OS LED stream is authoritative.
            const bool value = event.value != 0;
            if (!_scrollLedInitialKnown || _scrollLedAuthoritative || value != _initialScrollLedState)
            {
                _scrollLedAuthoritative = true;
                _state.ScrollLockActive = value;
            }
        }

        bool drainDevices()
        {
            for (const auto& device : _devices)
            {
                for (;;)
                {
                    input_event event{};
                    const ssize_t count = ::read(device.Fd, &event, sizeof(event));
                    if (count == static_cast<ssize_t>(sizeof(event)))
                    {
                        if (event.type == EV_KEY)
                            handleKey(event);
                        else if (event.type == EV_LED)
                            handleLed(event);
                        continue;
                    }
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        break;
                    if (count == 0 || (count < 0 && errno == ENODEV))
                        return false;
                    if (count < 0)
                        break;
                }
            }
            return true;
        }

        void run()
        {
            auto nextScan = std::chrono::steady_clock::now();
            while (_running.load())
            {
                const auto current = std::chrono::steady_clock::now();
                if (_devices.empty())
                {
                    if (current >= nextScan)
                    {
                        scanDevices();
                        nextScan = current + std::chrono::seconds(1);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                if (!drainDevices())
                {
                    closeDevices();
                    resetKeyState();
                    std::lock_guard lock(_mutex);
                    _connected = false;
                    _status = "evdev device disconnected; rescanning";
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        mutable std::mutex _mutex;
        std::atomic<bool> _running{false};
        std::atomic<bool> _restoreRequested{false};
        std::thread _thread;
        std::vector<Device> _devices;
        std::array<bool, KEY_MAX + 1> _keyDown{};
        ReactiveKeyState _state{};
        bool _connected = false;
        std::string _deviceName;
        std::string _status = "evdev not started";
        bool _hasCapsLedNode = false;
        bool _hasScrollLedNode = false;
        bool _scrollLedInitialKnown = false;
        bool _initialScrollLedState = false;
        bool _scrollLedAuthoritative = false;
        double _glfwBaseTime = 0.0;
        std::chrono::steady_clock::time_point _steadyBaseTime{};
    };

    class AppCpuMeter
    {
    public:
        float update(const double wallTime) noexcept
        {
            const std::clock_t cpuNow = std::clock();
            if (_lastWallTime < 0.0)
            {
                _lastWallTime = wallTime;
                _lastCpuTime = cpuNow;
                return _usage;
            }

            const double wallDelta = wallTime - _lastWallTime;
            if (wallDelta < 0.25)
                return _usage;

            const double cpuDelta = static_cast<double>(cpuNow - _lastCpuTime) / CLOCKS_PER_SEC;
            _usage = static_cast<float>(std::max(0.0, cpuDelta / wallDelta * 100.0));
            _lastWallTime = wallTime;
            _lastCpuTime = cpuNow;
            return _usage;
        }

    private:
        double _lastWallTime = -1.0;
        std::clock_t _lastCpuTime = 0;
        float _usage = 0.0f;
    };

    struct VisualizerSettings
    {
        bool Enabled = true;
        bool SendFramebuffer = true;
        bool MediaArtworkColor = true;
        bool ForceFullRow = true;
        bool ShowFramebuffer = true;
        bool ShowAnalysisSpectrum = true;
        bool ShowMappedSpectrum = true;
        bool LimitMainLoop = true;
        bool AutoReconnect = true;
        bool ShaderRecompileOnChange = false;
        bool ShaderKeyStateUniforms = true;
        bool ShaderCapsLockColorEnabled = true;
        bool ShaderScrollLockColorEnabled = true;
        int ShaderFramebufferWidth = DefaultShaderWidth;
        int ShaderFramebufferHeight = DefaultShaderHeight;
        int FrameRate = 240;
        int AnalysisBandCount = 512;
        int BassColumns = 3;
        int BassEndBand = 16;
        int FullRow = 5;
        float OverallGain = 1.62f;
        bool AutomaticOverallGain = false;
        float AutoGainBaseline = 1.62f;
        float AutoGainTargetRms = 0.10f;
        float AutoGainAdaptation = 0.35f;
        float AutoGainMinCorrection = 0.45f;
        float AutoGainMaxCorrection = 3.00f;
        float AutoGainSilenceGate = 0.0025f;
        float GlobalBrightness = 1.0f;
        float LiveOutputInterpolation = 0.35f;
        float WaveSpeed = 0.40f;
        float FeatherRows = 2.5f;
        float Saturation = 2.0f;
        float AttackSpeed = 3.5f;
        float ReleaseSpeed = 40.5f;
        float BassActivationThreshold = 0.65f;
        float BassMaxBoost = 1.68f;
        float ColorTransitionSpeed = 1.5f;
        float MediaColorBlend = 1.0f;
        float MinFrequency = 50.0f;
        float MaxFrequency = 16000.0f;
        float MinDb = -72.0f;
        float MaxDb = -15.0f;
        float StatisticsInterval = 0.20f;
        float MediaPollInterval = 0.50f;
        float ShaderEditorZoom = 1.0f;
        float ShaderTransitionSeconds = 0.35f;
        std::array<float, 3> SolidColor{1.0f, 0.0f, 0.0f};
        std::array<float, 3> ShaderCapsLockColor{0.10f, 0.80f, 1.00f};
        std::array<float, 3> ShaderScrollLockColor{1.00f, 0.20f, 0.55f};
        std::array<float, Columns> ColumnGain{0.55f, 0.58f, 0.56f, 0.72f, 0.78f, 0.72f, 0.81f, 0.74f, 0.77f, 0.84f, 0.84f, 0.86f, 0.99f, 0.99f, 0.99f, 0.92f};
        char AudioSource[128] = "easyeffects_sink.monitor";
        int BaseColorMode = 0;
        int ShaderDownsampleMode = 0;
        int ShaderPresetIndex = 1;
    };

    enum class ViewPage : std::uint8_t
    {
        Main,
        ShaderEditor
    };

    struct ShaderEditorState
    {
        TextEditor Vertex;
        TextEditor Fragment;
        int ActiveStage = 0;
        bool Initialized = false;
        bool ZoomInWasDown = false;
        bool ZoomOutWasDown = false;
        bool ZoomResetWasDown = false;
    };

    static TextEditor::Palette shaderEditorPalette()
    {
        auto palette = TextEditor::GetDarkPalette();
        palette[static_cast<std::size_t>(TextEditor::Color::background)] = IM_COL32(13, 13, 13, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] = IM_COL32(90, 90, 90, 110);
        palette[static_cast<std::size_t>(TextEditor::Color::lineNumber)] = IM_COL32(105, 105, 105, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::currentLineNumber)] = IM_COL32(230, 230, 230, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::matchingBracketBackground)] = IM_COL32(80, 80, 80, 100);
        return palette;
    }

    static void configureShaderEditor(TextEditor& editor, const std::string_view source)
    {
        editor.SetPalette(shaderEditorPalette());
        editor.SetLanguage(TextEditor::Language::Glsl());
        editor.SetTabSize(4);
        editor.SetInsertSpacesOnTabs(true);
        editor.SetAutoIndentEnabled(true);
        editor.SetShowLineNumbersEnabled(true);
        editor.SetShowMiniMapEnabled(true);
        editor.SetMiniMapColumns(28);
        editor.SetShowMatchingBrackets(true);
        editor.SetCompletePairedGlyphs(true);
        editor.SetLineFoldingEnabled(true);
        editor.SetWordWrapEnabled(false);
        editor.SetText(source);
    }

    static void initializeShaderEditors(ShaderEditorState& state, const std::string_view vertexSource, const std::string_view fragmentSource)
    {
        if (state.Initialized)
            return;
        configureShaderEditor(state.Vertex, vertexSource);
        configureShaderEditor(state.Fragment, fragmentSource);
        state.Initialized = true;
    }



    static std::string g_SettingsStatus = "Settings not loaded yet";

    static std::filesystem::path settingsPath()
    {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "quartz" / "visualizer.ini";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "quartz" / "visualizer.ini";
        return "quartz_visualizer.ini";
    }

    static std::filesystem::path vertexShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.vert";
    }

    static std::filesystem::path fragmentShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.frag";
    }

    static std::filesystem::path shaderMaterialPath()
    {
        return settingsPath().parent_path() / "visualizer.material.ini";
    }

    static void setShaderSource(std::array<char, ShaderSourceCapacity>& destination, const std::string_view source)
    {
        const std::size_t size = std::min(source.size(), destination.size() - 1);
        std::memcpy(destination.data(), source.data(), size);
        destination[size] = '\0';
    }

    static bool loadTextFile(const std::filesystem::path& path, std::array<char, ShaderSourceCapacity>& source)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) >= source.size())
            return false;
        file.seekg(0);
        file.read(source.data(), size);
        if (!file)
            return false;
        source[static_cast<std::size_t>(size)] = '\0';
        return true;
    }

    static bool saveTextFile(const std::filesystem::path& path, const std::array<char, ShaderSourceCapacity>& source)
    {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(source.data(), static_cast<std::streamsize>(std::strlen(source.data())));
        return static_cast<bool>(file);
    }

    static bool replaceAll(std::string& value, const std::string_view from, const std::string_view to)
    {
        if (from.empty())
            return false;
        bool changed = false;
        std::size_t offset = 0;
        while ((offset = value.find(from, offset)) != std::string::npos)
        {
            value.replace(offset, from.size(), to);
            offset += to.size();
            changed = true;
        }
        return changed;
    }

    static bool migrateObsoleteShaderSource(std::array<char, ShaderSourceCapacity>& source)
    {
        std::string value(source.data());
        const std::string oldState = std::string("u") + "C" + "trl";
        const std::string oldColor = oldState + "Color";
        const std::string oldEnabled = oldColor + "Enabled";
        bool changed = false;
        changed |= replaceAll(value, oldEnabled, "uCapsLockColorEnabled");
        changed |= replaceAll(value, oldColor, "uCapsLockColor");
        changed |= replaceAll(value, oldState, "uCapsLock");
        changed |= replaceAll(value, "row == 5 && (column == 0 || column == 12)", "row == 3 && column == 0");
        changed |= replaceAll(value, "keyRow == 5 && (keyColumn == 0 || keyColumn == 12)", "keyRow == 3 && keyColumn == 0");
        if (changed)
            setShaderSource(source, value);
        return changed;
    }

    static void loadShaderSources(std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        setShaderSource(vertexSource, DefaultVertexShaderSource);
        setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
        loadTextFile(vertexShaderPath(), vertexSource);
        loadTextFile(fragmentShaderPath(), fragmentSource);
    }

    static int detectShaderPreset(const std::string_view source) noexcept
    {
        for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            if (source == std::string_view(ShaderPresets[i].FragmentSource)) return static_cast<int>(i + 1);
        return 0;
    }

    static void saveShaderSources(const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        saveTextFile(vertexShaderPath(), vertexSource);
        saveTextFile(fragmentShaderPath(), fragmentSource);
    }

    static std::string serializeSettings(const VisualizerSettings& settings)
    {
        std::ostringstream stream;
        stream << std::boolalpha << std::setprecision(9);
        stream << "Enabled=" << settings.Enabled << '\n';
        stream << "SendFramebuffer=" << settings.SendFramebuffer << '\n';
        stream << "MediaArtworkColor=" << settings.MediaArtworkColor << '\n';
        stream << "ForceFullRow=" << settings.ForceFullRow << '\n';
        stream << "ShowFramebuffer=" << settings.ShowFramebuffer << '\n';
        stream << "ShowAnalysisSpectrum=" << settings.ShowAnalysisSpectrum << '\n';
        stream << "ShowMappedSpectrum=" << settings.ShowMappedSpectrum << '\n';
        stream << "LimitMainLoop=" << settings.LimitMainLoop << '\n';
        stream << "AutoReconnect=" << settings.AutoReconnect << '\n';
        stream << "ShaderRecompileOnChange=" << settings.ShaderRecompileOnChange << '\n';
        stream << "ShaderKeyStateUniforms=" << settings.ShaderKeyStateUniforms << '\n';
        stream << "ShaderCapsLockColorEnabled=" << settings.ShaderCapsLockColorEnabled << '\n';
        stream << "ShaderScrollLockColorEnabled=" << settings.ShaderScrollLockColorEnabled << '\n';
        stream << "ShaderFramebufferWidth=" << settings.ShaderFramebufferWidth << '\n';
        stream << "ShaderFramebufferHeight=" << settings.ShaderFramebufferHeight << '\n';
        stream << "FrameRate=" << settings.FrameRate << '\n';
        stream << "AnalysisBandCount=" << settings.AnalysisBandCount << '\n';
        stream << "BassColumns=" << settings.BassColumns << '\n';
        stream << "BassEndBand=" << settings.BassEndBand << '\n';
        stream << "FullRow=" << settings.FullRow << '\n';
        stream << "OverallGain=" << settings.OverallGain << '\n';
        stream << "AutomaticOverallGain=" << settings.AutomaticOverallGain << '\n';
        stream << "AutoGainBaseline=" << settings.AutoGainBaseline << '\n';
        stream << "AutoGainTargetRms=" << settings.AutoGainTargetRms << '\n';
        stream << "AutoGainAdaptation=" << settings.AutoGainAdaptation << '\n';
        stream << "AutoGainMinCorrection=" << settings.AutoGainMinCorrection << '\n';
        stream << "AutoGainMaxCorrection=" << settings.AutoGainMaxCorrection << '\n';
        stream << "AutoGainSilenceGate=" << settings.AutoGainSilenceGate << '\n';
        stream << "GlobalBrightness=" << settings.GlobalBrightness << '\n';
        stream << "LiveOutputInterpolation=" << settings.LiveOutputInterpolation << '\n';
        stream << "WaveSpeed=" << settings.WaveSpeed << '\n';
        stream << "FeatherRows=" << settings.FeatherRows << '\n';
        stream << "Saturation=" << settings.Saturation << '\n';
        stream << "AttackSpeed=" << settings.AttackSpeed << '\n';
        stream << "ReleaseSpeed=" << settings.ReleaseSpeed << '\n';
        stream << "BassActivationThreshold=" << settings.BassActivationThreshold << '\n';
        stream << "BassMaxBoost=" << settings.BassMaxBoost << '\n';
        stream << "ColorTransitionSpeed=" << settings.ColorTransitionSpeed << '\n';
        stream << "MediaColorBlend=" << settings.MediaColorBlend << '\n';
        stream << "MinFrequency=" << settings.MinFrequency << '\n';
        stream << "MaxFrequency=" << settings.MaxFrequency << '\n';
        stream << "MinDb=" << settings.MinDb << '\n';
        stream << "MaxDb=" << settings.MaxDb << '\n';
        stream << "StatisticsInterval=" << settings.StatisticsInterval << '\n';
        stream << "MediaPollInterval=" << settings.MediaPollInterval << '\n';
        stream << "ShaderEditorZoom=" << settings.ShaderEditorZoom << '\n';
        stream << "ShaderTransitionSeconds=" << settings.ShaderTransitionSeconds << '\n';
        stream << "BaseColorMode=" << settings.BaseColorMode << '\n';
        stream << "ShaderDownsampleMode=" << settings.ShaderDownsampleMode << '\n';
        stream << "ShaderPresetIndex=" << settings.ShaderPresetIndex << '\n';
        stream << "AudioSource=" << settings.AudioSource << '\n';
        stream << "SolidColor=" << settings.SolidColor[0] << ',' << settings.SolidColor[1] << ',' << settings.SolidColor[2] << '\n';
        stream << "ShaderCapsLockColor=" << settings.ShaderCapsLockColor[0] << ',' << settings.ShaderCapsLockColor[1] << ',' << settings.ShaderCapsLockColor[2] << '\n';
        stream << "ShaderScrollLockColor=" << settings.ShaderScrollLockColor[0] << ',' << settings.ShaderScrollLockColor[1] << ',' << settings.ShaderScrollLockColor[2] << '\n';
        stream << "ColumnGain=";
        for (std::size_t i = 0; i < settings.ColumnGain.size(); ++i)
        {
            if (i != 0)
                stream << ',';
            stream << settings.ColumnGain[i];
        }
        stream << '\n';
        return stream.str();
    }

    static std::string_view trimSettingValue(std::string_view value) noexcept
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    static bool parseBool(const std::string_view value, bool& result)
    {
        if (value == "true" || value == "1") { result = true; return true; }
        if (value == "false" || value == "0") { result = false; return true; }
        return false;
    }

    template<typename T>
    static bool parseNumber(const std::string_view value, T& result)
    {
        T parsed{};
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars_result parsedResult{};
        if constexpr (std::is_integral_v<T>) parsedResult = std::from_chars(begin, end, parsed);
        else parsedResult = std::from_chars(begin, end, parsed, std::chars_format::general);
        if (parsedResult.ec != std::errc{} || parsedResult.ptr != end) return false;
        if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(parsed)) return false;
        result = parsed;
        return true;
    }

    template<std::size_t N>
    static bool parseFloatArray(const std::string_view value, std::array<float, N>& result)
    {
        std::array<float, N> parsed{};
        std::size_t start = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = trimSettingValue(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
            if (!parseNumber(part, parsed[i]))
                return false;
            if (i + 1 < N)
            {
                if (end == std::string_view::npos)
                    return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    static void loadSettings(VisualizerSettings& settings)
    {
        const auto path = settingsPath();
        std::ifstream file(path);
        if (!file)
        {
            g_SettingsStatus = "No settings file; using defaults";
            return;
        }

        std::size_t loaded = 0;
        std::size_t failed = 0;
        std::string line;
        while (std::getline(file, line))
        {
            std::string_view whole = trimSettingValue(line);
            if (whole.empty() || whole.front() == '#' || whole.front() == ';') continue;
            const std::size_t separator = whole.find('=');
            if (separator == std::string_view::npos) { ++failed; continue; }
            std::string_view key = trimSettingValue(whole.substr(0, separator));
            const std::string_view value = trimSettingValue(whole.substr(separator + 1));
            if (key.size() >= 3 && static_cast<unsigned char>(key[0]) == 0xEF && static_cast<unsigned char>(key[1]) == 0xBB && static_cast<unsigned char>(key[2]) == 0xBF) key.remove_prefix(3);

#define LOAD_BOOL(name) if (key == #name) { if (parseBool(value, settings.name)) ++loaded; else ++failed; continue; }
#define LOAD_NUM(name) if (key == #name) { if (parseNumber(value, settings.name)) ++loaded; else ++failed; continue; }
            LOAD_BOOL(Enabled)
            LOAD_BOOL(SendFramebuffer)
            LOAD_BOOL(MediaArtworkColor)
            LOAD_BOOL(ForceFullRow)
            LOAD_BOOL(ShowFramebuffer)
            LOAD_BOOL(ShowAnalysisSpectrum)
            LOAD_BOOL(ShowMappedSpectrum)
            LOAD_BOOL(LimitMainLoop)
            LOAD_BOOL(AutoReconnect)
            LOAD_BOOL(ShaderRecompileOnChange)
            LOAD_BOOL(ShaderKeyStateUniforms)
            LOAD_BOOL(ShaderCapsLockColorEnabled)
            LOAD_BOOL(ShaderScrollLockColorEnabled)
            LOAD_NUM(ShaderFramebufferWidth)
            LOAD_NUM(ShaderFramebufferHeight)
            LOAD_NUM(FrameRate)
            LOAD_NUM(AnalysisBandCount)
            LOAD_NUM(BassColumns)
            LOAD_NUM(BassEndBand)
            LOAD_NUM(FullRow)
            LOAD_NUM(OverallGain)
            LOAD_BOOL(AutomaticOverallGain)
            LOAD_NUM(AutoGainBaseline)
            LOAD_NUM(AutoGainTargetRms)
            LOAD_NUM(AutoGainAdaptation)
            LOAD_NUM(AutoGainMinCorrection)
            LOAD_NUM(AutoGainMaxCorrection)
            LOAD_NUM(AutoGainSilenceGate)
            LOAD_NUM(GlobalBrightness)
            LOAD_NUM(LiveOutputInterpolation)
            LOAD_NUM(WaveSpeed)
            LOAD_NUM(FeatherRows)
            LOAD_NUM(Saturation)
            LOAD_NUM(AttackSpeed)
            LOAD_NUM(ReleaseSpeed)
            LOAD_NUM(BassActivationThreshold)
            LOAD_NUM(BassMaxBoost)
            LOAD_NUM(ColorTransitionSpeed)
            LOAD_NUM(MediaColorBlend)
            LOAD_NUM(MinFrequency)
            LOAD_NUM(MaxFrequency)
            LOAD_NUM(MinDb)
            LOAD_NUM(MaxDb)
            LOAD_NUM(StatisticsInterval)
            LOAD_NUM(MediaPollInterval)
            LOAD_NUM(ShaderEditorZoom)
            LOAD_NUM(ShaderTransitionSeconds)
            LOAD_NUM(BaseColorMode)
            LOAD_NUM(ShaderDownsampleMode)
#undef LOAD_NUM
#undef LOAD_BOOL
            if (key == "StartMinimized" || key == "StartHidden") { ++loaded; continue; }
            // ShaderPresetIndex is deliberately ignored. The fragment file is authoritative and
            // the preset is detected from its contents after loading, so preset reordering cannot
            // silently replace a user's saved shader.
            if (key == "ShaderPresetIndex") { ++loaded; continue; }
            if (key == "AudioSource")
            {
                const std::size_t count = std::min(value.size(), sizeof(settings.AudioSource) - 1);
                std::memcpy(settings.AudioSource, value.data(), count);
                settings.AudioSource[count] = '\0';
                ++loaded;
            }
            else if (key == "SolidColor") { parseFloatArray(value, settings.SolidColor) ? ++loaded : ++failed; }
            else if (key == "ShaderCapsLockColor") { parseFloatArray(value, settings.ShaderCapsLockColor) ? ++loaded : ++failed; }
            else if (key == "ShaderScrollLockColor") { parseFloatArray(value, settings.ShaderScrollLockColor) ? ++loaded : ++failed; }
            else if (key == "ColumnGain") { parseFloatArray(value, settings.ColumnGain) ? ++loaded : ++failed; }
        }
        settings.FrameRate = std::clamp(settings.FrameRate, 30, 500);
        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassColumns = std::clamp(settings.BassColumns, 2, 8);
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.FullRow = std::clamp(settings.FullRow, 0, static_cast<int>(Rows) - 2);
        settings.BaseColorMode = std::clamp(settings.BaseColorMode, 0, 2);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.ShaderTransitionSeconds = std::clamp(settings.ShaderTransitionSeconds, 0.0f, 10.0f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.LiveOutputInterpolation = std::clamp(settings.LiveOutputInterpolation, 0.0f, 1.0f);
        g_SettingsStatus = "Loaded " + std::to_string(loaded) + " settings from " + path.string();
        if (failed != 0) g_SettingsStatus += " (" + std::to_string(failed) + " malformed lines ignored)";
    }

    static bool saveSettings(const VisualizerSettings& settings)
    {
        const auto path = settingsPath();
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
        {
            std::ofstream file(temporaryPath, std::ios::trunc);
            if (!file) return false;
            file << serializeSettings(settings);
            if (!file) { file.close(); std::filesystem::remove(temporaryPath, error); return false; }
        }
        const auto backupPath = std::filesystem::path(path.string() + ".bak");
        if (std::filesystem::exists(path, error))
        {
            error.clear();
            std::filesystem::copy_file(path, backupPath, std::filesystem::copy_options::overwrite_existing, error);
            error.clear();
        }
        std::filesystem::rename(temporaryPath, path, error);
        if (error) { std::filesystem::remove(temporaryPath, error); return false; }
        return true;
    }

    struct HSV
    {
        float H = 0.0f;
        float S = 0.0f;
        float V = 0.0f;
    };

    static HSV rgbToHsv(const Color32 color) noexcept
    {
        const float r = color.R / 255.0f;
        const float g = color.G / 255.0f;
        const float b = color.B / 255.0f;
        const float maximum = std::max({r, g, b});
        const float minimum = std::min({r, g, b});
        const float delta = maximum - minimum;
        HSV hsv{.S = maximum > 0.0f ? delta / maximum : 0.0f, .V = maximum};
        if (delta <= 0.00001f)
            return hsv;
        if (maximum == r)
            hsv.H = std::fmod((g - b) / delta, 6.0f);
        else if (maximum == g)
            hsv.H = (b - r) / delta + 2.0f;
        else
            hsv.H = (r - g) / delta + 4.0f;
        hsv.H /= 6.0f;
        if (hsv.H < 0.0f)
            hsv.H += 1.0f;
        return hsv;
    }

    static Color32 hsvToRgb(float h, const float s, const float v) noexcept
    {
        h -= std::floor(h);
        const float x = h * 6.0f;
        const int sector = static_cast<int>(x);
        const float fraction = x - static_cast<float>(sector);
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - fraction * s);
        const float t = v * (1.0f - (1.0f - fraction) * s);
        float r;
        float g;
        float b;
        switch (sector % 6)
        {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
        return {
            static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f))
        };
    }

    static Color32 lerpColor(const Color32 from, const Color32 to, const float amount) noexcept
    {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        const auto a = rgbToHsv(from);
        const auto b = rgbToHsv(to);
        float hueDelta = b.H - a.H;
        if (hueDelta > 0.5f)
            hueDelta -= 1.0f;
        else if (hueDelta < -0.5f)
            hueDelta += 1.0f;
        float h = a.H + hueDelta * t;
        h -= std::floor(h);
        return hsvToRgb(h, a.S + (b.S - a.S) * t, a.V + (b.V - a.V) * t);
    }

    static Color32 floatColor(const std::array<float, 3>& color) noexcept
    {
        return {
            static_cast<std::uint8_t>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f))
        };
    }

    static void saturate(float& r, float& g, float& b, const float saturation) noexcept
    {
        const float gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
        r = std::clamp(gray + (r - gray) * saturation, 0.0f, 255.0f);
        g = std::clamp(gray + (g - gray) * saturation, 0.0f, 255.0f);
        b = std::clamp(gray + (b - gray) * saturation, 0.0f, 255.0f);
    }


    struct ShaderUniformMetadata
    {
        bool Explicit = false;
        bool Hidden = false;
        bool Color = false;
        bool HasDefault = false;
        bool HasMin = false;
        bool HasMax = false;
        bool HasStep = false;
        std::string Label;
        std::string Id;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.01f;
        std::array<float, 4> Default{};
    };

    struct ShaderMaterialParameter
    {
        std::string Name;
        std::string Label;
        std::string PersistenceKey;
        GLenum Type = 0;
        GLint Location = -1;
        int Components = 1;
        bool Integer = false;
        bool Boolean = false;
        bool Color = false;
        bool HasMin = false;
        bool HasMax = false;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.01f;
        std::array<float, 4> FloatValue{};
        std::array<float, 4> FloatDefault{};
        std::array<int, 4> IntValue{};
        std::array<int, 4> IntDefault{};
    };

    static std::unordered_map<std::string, std::string> g_ShaderMaterialValues;

    static std::string shaderMaterialSerializeFloats(const std::array<float, 4>& values, const int count)
    {
        std::ostringstream stream;
        stream << std::setprecision(9);
        for (int i = 0; i < count; ++i)
        {
            if (i != 0) stream << ',';
            stream << values[static_cast<std::size_t>(i)];
        }
        return stream.str();
    }

    static std::string shaderMaterialSerializeInts(const std::array<int, 4>& values, const int count)
    {
        std::ostringstream stream;
        for (int i = 0; i < count; ++i)
        {
            if (i != 0) stream << ',';
            stream << values[static_cast<std::size_t>(i)];
        }
        return stream.str();
    }

    static bool parseShaderMaterialFloats(const std::string_view value, std::array<float, 4>& result, const int count)
    {
        std::array<float, 4> parsed{};
        std::size_t start = 0;
        for (int i = 0; i < count; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!parseNumber(part, parsed[static_cast<std::size_t>(i)])) return false;
            if (i + 1 < count)
            {
                if (end == std::string_view::npos) return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    static bool parseShaderMaterialInts(const std::string_view value, std::array<int, 4>& result, const int count)
    {
        std::array<int, 4> parsed{};
        std::size_t start = 0;
        for (int i = 0; i < count; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!parseNumber(part, parsed[static_cast<std::size_t>(i)])) return false;
            if (i + 1 < count)
            {
                if (end == std::string_view::npos) return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    static void loadShaderMaterialValueCache()
    {
        g_ShaderMaterialValues.clear();
        std::ifstream file(shaderMaterialPath());
        std::string line;
        while (std::getline(file, line))
        {
            const std::string_view whole = trimSettingValue(line);
            if (whole.empty() || whole.front() == '#' || whole.front() == ';') continue;
            const std::size_t separator = whole.find('=');
            if (separator == std::string_view::npos) continue;
            const std::string key(trimSettingValue(whole.substr(0, separator)));
            const std::string value(trimSettingValue(whole.substr(separator + 1)));
            if (!key.empty()) g_ShaderMaterialValues[key] = value;
        }
    }

    static bool saveShaderMaterialValueCache()
    {
        const auto path = shaderMaterialPath();
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
        std::ofstream file(temporaryPath, std::ios::trunc);
        if (!file) return false;
        file << "# Quartz reflected shader material parameters\n";
        std::vector<std::string> keys;
        keys.reserve(g_ShaderMaterialValues.size());
        for (const auto& [key, _] : g_ShaderMaterialValues) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) file << key << '=' << g_ShaderMaterialValues[key] << '\n';
        file.close();
        if (!file) { std::filesystem::remove(temporaryPath, error); return false; }
        std::filesystem::rename(temporaryPath, path, error);
        if (error)
        {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporaryPath, path, error);
        }
        return !error;
    }

    static std::string prettyUniformLabel(std::string_view name)
    {
        if (name.size() > 1 && name[0] == 'u' && std::isupper(static_cast<unsigned char>(name[1]))) name.remove_prefix(1);
        std::string result;
        result.reserve(name.size() + 8);
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const char c = name[i];
            if (i != 0 && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1]))) result.push_back(' ');
            result.push_back(c);
        }
        if (!result.empty()) result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
        return result;
    }

    static bool isEngineShaderUniform(std::string_view name)
    {
        if (const std::size_t bracket = name.find('['); bracket != std::string_view::npos) name = name.substr(0, bracket);
        static constexpr std::array<std::string_view, 20> Names{
            "uTime", "uResolution", "uBands", "uMediaColor", "uMediaAmount", "uSolidColor", "uWaveSpeed", "uFeatherRows", "uSaturation",
            "uForceFullRow", "uFullRow", "uCapsLock", "uScrollLock", "uCapsLockColor", "uScrollLockColor", "uCapsLockColorEnabled",
            "uScrollLockColorEnabled", "uKeyState", "uKeyEvents", "gl_DepthRange"
        };
        return std::find(Names.begin(), Names.end(), name) != Names.end();
    }

    static std::unordered_map<std::string, ShaderUniformMetadata> parseShaderUniformMetadata(const std::string_view vertexSource, const std::string_view fragmentSource)
    {
        std::unordered_map<std::string, ShaderUniformMetadata> result;
        static const std::regex uniformPattern(R"rx(\buniform\s+[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;)rx");
        static const std::regex labelPattern(R"rx(label\s*=\s*"([^"]*)")rx", std::regex::icase);
        static const std::regex idPattern(R"(\bid\s*=\s*([A-Za-z0-9_.:-]+))", std::regex::icase);
        const auto parseSource = [&](const std::string_view source)
        {
            std::size_t offset = 0;
            while (offset < source.size())
            {
                const std::size_t end = source.find('\n', offset);
                const std::string line(source.substr(offset, end == std::string_view::npos ? source.size() - offset : end - offset));
                std::smatch match;
                if (std::regex_search(line, match, uniformPattern))
                {
                    const std::size_t uiOffset = line.find("@ui");
                    if (uiOffset != std::string::npos)
                    {
                        ShaderUniformMetadata metadata{};
                        metadata.Explicit = true;
                        const std::string options = line.substr(uiOffset + 3);
                        metadata.Hidden = std::regex_search(options, std::regex(R"(\bhidden\b)", std::regex::icase));
                        metadata.Color = std::regex_search(options, std::regex(R"(\bcolor\b)", std::regex::icase));
                        std::smatch optionMatch;
                        if (std::regex_search(options, optionMatch, labelPattern)) metadata.Label = optionMatch[1].str();
                        if (std::regex_search(options, optionMatch, idPattern)) metadata.Id = optionMatch[1].str();

                        const auto findNumber = [&](const char* key, float& destination, bool& present)
                        {
                            const std::regex pattern(std::string(R"rx(\b)rx") + key + R"rx(\s*=\s*([-+]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][-+]?[0-9]+)?))rx", std::regex::icase);
                            std::smatch numberMatch;
                            if (!std::regex_search(options, numberMatch, pattern)) return;
                            if (parseNumber(numberMatch[1].str(), destination)) present = true;
                        };
                        findNumber("min", metadata.Min, metadata.HasMin);
                        findNumber("max", metadata.Max, metadata.HasMax);
                        findNumber("step", metadata.Step, metadata.HasStep);

                        const std::regex defaultPattern(R"(\bdefault\s*=\s*([^\s]+))", std::regex::icase);
                        if (std::regex_search(options, optionMatch, defaultPattern))
                        {
                            metadata.HasDefault = parseShaderMaterialFloats(optionMatch[1].str(), metadata.Default, 1);
                            if (!metadata.HasDefault)
                            {
                                std::array<float, 4> values{};
                                for (int count = 4; count >= 2 && !metadata.HasDefault; --count)
                                {
                                    if (parseShaderMaterialFloats(optionMatch[1].str(), values, count))
                                    {
                                        metadata.Default = values;
                                        metadata.HasDefault = true;
                                    }
                                }
                            }
                        }
                        result[match[1].str()] = std::move(metadata);
                    }
                }
                if (end == std::string_view::npos) break;
                offset = end + 1;
            }
        };
        parseSource(vertexSource);
        parseSource(fragmentSource);
        return result;
    }

    static int shaderUniformComponents(const GLenum type) noexcept
    {
        switch (type)
        {
        case GL_FLOAT:
        case GL_INT:
        case GL_BOOL: return 1;
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_BOOL_VEC2: return 2;
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_BOOL_VEC3: return 3;
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_BOOL_VEC4: return 4;
        default: return 0;
        }
    }

    static const char* shaderUniformTypeName(const GLenum type) noexcept
    {
        switch (type)
        {
        case GL_FLOAT: return "float";
        case GL_FLOAT_VEC2: return "vec2";
        case GL_FLOAT_VEC3: return "vec3";
        case GL_FLOAT_VEC4: return "vec4";
        case GL_INT: return "int";
        case GL_INT_VEC2: return "ivec2";
        case GL_INT_VEC3: return "ivec3";
        case GL_INT_VEC4: return "ivec4";
        case GL_BOOL: return "bool";
        case GL_BOOL_VEC2: return "bvec2";
        case GL_BOOL_VEC3: return "bvec3";
        case GL_BOOL_VEC4: return "bvec4";
        default: return "unsupported";
        }
    }

    class ShaderFramebuffer
    {
    public:
        ~ShaderFramebuffer() { shutdown(); }

        bool initialize(const int width = DefaultShaderWidth, const int height = DefaultShaderHeight) noexcept
        {
            if (_framebuffer == 0)
            {
                glGenFramebuffers(1, &_framebuffer);
                glGenTextures(1, &_texture);
                glGenVertexArrays(1, &_vao);
                if (_framebuffer == 0 || _texture == 0 || _vao == 0)
                {
                    _status = "Failed to create OpenGL shader framebuffer objects";
                    shutdown();
                    return false;
                }
                glBindTexture(GL_TEXTURE_2D, _texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            if (_width == width && _height == height && !_pixels.empty())
                return true;
            return regenerate(width, height);
        }

        bool regenerate(int width, int height) noexcept
        {
            width = std::clamp(width, static_cast<int>(Columns), MaxShaderDimension);
            height = std::clamp(height, static_cast<int>(Rows), MaxShaderDimension);
            if (_framebuffer == 0)
                return initialize(width, height);

            GLint previousFramebuffer = 0;
            GLint previousTexture = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

            glBindTexture(GL_TEXTURE_2D, _texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                _status = "Shader framebuffer is incomplete";
                return false;
            }
            _width = width;
            _height = height;
            _pixels.resize(static_cast<std::size_t>(_width) * _height * 4);
            _status = "Shader framebuffer regenerated";
            return true;
        }

        bool compile(const std::string_view vertexSource, const std::string_view fragmentSource)
        {
            if (!initialize(_width > 0 ? _width : DefaultShaderWidth, _height > 0 ? _height : DefaultShaderHeight))
                return false;
            const GLuint vertex = compileStage(GL_VERTEX_SHADER, vertexSource);
            if (vertex == 0)
                return false;
            const GLuint fragment = compileStage(GL_FRAGMENT_SHADER, fragmentSource);
            if (fragment == 0)
            {
                glDeleteShader(vertex);
                return false;
            }

            const GLuint program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);

            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE)
            {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
                glGetProgramInfoLog(program, length, nullptr, log.data());
                _status = "Link error:\n" + log;
                glDeleteProgram(program);
                return false;
            }

            stashMaterialValues();
            if (_program != 0)
                glDeleteProgram(_program);
            _program = program;
            reflectMaterialParameters(vertexSource, fragmentSource);
            _status = "Shaders compiled successfully";
            return true;
        }

        bool render(const double time, const std::array<float, Columns>& bands, const VisualizerSettings& settings, const std::optional<Color32> mediaColor, const float mediaAmount, const ReactiveKeyState& reactiveKeys, std::array<Color32, MatrixSize>& framebuffer)
        {
            if (_program == 0 || _framebuffer == 0 || _width <= 0 || _height <= 0)
                return false;

            GLint previousFramebuffer = 0;
            GLint previousProgram = 0;
            GLint previousVao = 0;
            GLint previousViewport[4]{};
            GLfloat previousClearColor[4]{};
            GLint previousPackAlignment = 4;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
            glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
            const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
            const GLboolean ditherEnabled = glIsEnabled(GL_DITHER);
            const GLboolean framebufferSrgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);

            glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
            glViewport(0, 0, _width, _height);
            glDisable(GL_BLEND);
            glDisable(GL_DITHER);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(_program);
            glBindVertexArray(_vao);

            uniform1f("uTime", static_cast<float>(time));
            uniform2f("uResolution", static_cast<float>(_width), static_cast<float>(_height));
            const GLint bandsLocation = glGetUniformLocation(_program, "uBands");
            if (bandsLocation >= 0)
                glUniform1fv(bandsLocation, static_cast<GLsizei>(Columns), bands.data());
            const Color32 media = mediaColor.value_or(Color32{0, 0, 0});
            uniform3f("uMediaColor", media.R / 255.0f, media.G / 255.0f, media.B / 255.0f);
            uniform1f("uMediaAmount", std::clamp(mediaAmount * settings.MediaColorBlend, 0.0f, 1.0f));
            uniform3f("uSolidColor", settings.SolidColor[0], settings.SolidColor[1], settings.SolidColor[2]);
            uniform1f("uWaveSpeed", settings.WaveSpeed);
            uniform1f("uFeatherRows", settings.FeatherRows);
            uniform1f("uSaturation", settings.Saturation);
            uniform1i("uForceFullRow", settings.ForceFullRow ? 1 : 0);
            uniform1i("uFullRow", settings.FullRow);
            uniform1f("uCapsLock", settings.ShaderKeyStateUniforms && reactiveKeys.CapsLockActive ? 1.0f : 0.0f);
            uniform1f("uScrollLock", settings.ShaderKeyStateUniforms && reactiveKeys.ScrollLockActive ? 1.0f : 0.0f);
            const GLint keyStateLocation = glGetUniformLocation(_program, "uKeyState[0]");
            if (keyStateLocation >= 0)
            {
                if (settings.ShaderKeyStateUniforms)
                    glUniform1fv(keyStateLocation, static_cast<GLsizei>(MatrixSize), reactiveKeys.Down.data());
                else
                {
                    static constexpr std::array<float, MatrixSize> EmptyKeyState{};
                    glUniform1fv(keyStateLocation, static_cast<GLsizei>(MatrixSize), EmptyKeyState.data());
                }
            }
            const GLint keyEventsLocation = glGetUniformLocation(_program, "uKeyEvents[0]");
            if (keyEventsLocation >= 0)
            {
                std::array<float, ReactiveKeyState::EventCount * 4> events{};
                if (settings.ShaderKeyStateUniforms)
                {
                    for (std::size_t i = 0; i < reactiveKeys.Events.size(); ++i)
                    {
                        events[i * 4 + 0] = reactiveKeys.Events[i].Column;
                        events[i * 4 + 1] = reactiveKeys.Events[i].Row;
                        events[i * 4 + 2] = reactiveKeys.Events[i].Time;
                        events[i * 4 + 3] = reactiveKeys.Events[i].Valid;
                    }
                }
                glUniform4fv(keyEventsLocation, static_cast<GLsizei>(ReactiveKeyState::EventCount), events.data());
            }
            uniform3f("uCapsLockColor", settings.ShaderCapsLockColor[0], settings.ShaderCapsLockColor[1], settings.ShaderCapsLockColor[2]);
            uniform3f("uScrollLockColor", settings.ShaderScrollLockColor[0], settings.ShaderScrollLockColor[1], settings.ShaderScrollLockColor[2]);
            uniform1i("uCapsLockColorEnabled", settings.ShaderCapsLockColorEnabled ? 1 : 0);
            uniform1i("uScrollLockColorEnabled", settings.ShaderScrollLockColorEnabled ? 1 : 0);

            applyMaterialParameters();
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, _width, _height, GL_RGBA, GL_UNSIGNED_BYTE, _pixels.data());

            const auto samplePixel = [&](const int x, const int y) -> Color32
            {
                const int safeX = std::clamp(x, 0, _width - 1);
                const int safeY = std::clamp(y, 0, _height - 1);
                const std::size_t offset = (static_cast<std::size_t>(safeY) * _width + safeX) * 4;
                return {_pixels[offset + 0], _pixels[offset + 1], _pixels[offset + 2]};
            };
            const auto averageRegion = [&](const int x, const int y, const int width, const int height) -> Color32
            {
                const int safeWidth = std::max(width, 1);
                const int safeHeight = std::max(height, 1);
                std::uint64_t r = 0, g = 0, b = 0;
                for (int py = 0; py < safeHeight; ++py)
                {
                    for (int px = 0; px < safeWidth; ++px)
                    {
                        const auto color = samplePixel(x + px, y + py);
                        r += color.R;
                        g += color.G;
                        b += color.B;
                    }
                }
                const std::uint64_t samples = static_cast<std::uint64_t>(safeWidth) * safeHeight;
                return {
                    static_cast<std::uint8_t>((r + samples / 2) / samples),
                    static_cast<std::uint8_t>((g + samples / 2) / samples),
                    static_cast<std::uint8_t>((b + samples / 2) / samples)
                };
            };

            for (std::size_t row = 0; row < Rows; ++row)
            {
                // OpenGL's row zero is the bottom; logical framebuffer row zero is the top.
                const int invertedRow = static_cast<int>(Rows) - 1 - static_cast<int>(row);
                const int sourceY0 = invertedRow * _height / static_cast<int>(Rows);
                const int sourceY1 = (invertedRow + 1) * _height / static_cast<int>(Rows);
                const int blockHeight = std::max(sourceY1 - sourceY0, 1);
                for (std::size_t column = 0; column < Columns; ++column)
                {
                    const int sourceX0 = static_cast<int>(column) * _width / static_cast<int>(Columns);
                    const int sourceX1 = (static_cast<int>(column) + 1) * _width / static_cast<int>(Columns);
                    const int blockWidth = std::max(sourceX1 - sourceX0, 1);
                    switch (settings.ShaderDownsampleMode)
                    {
                    case 1:
                    {
                        const int sampleWidth = std::min(4, blockWidth);
                        const int sampleHeight = std::min(4, blockHeight);
                        const int sampleX = sourceX0 + (blockWidth - sampleWidth) / 2;
                        const int sampleY = sourceY0 + (blockHeight - sampleHeight) / 2;
                        framebuffer[row * Columns + column] = averageRegion(sampleX, sampleY, sampleWidth, sampleHeight);
                        break;
                    }
                    case 2:
                        framebuffer[row * Columns + column] = samplePixel(sourceX0 + blockWidth / 2, sourceY0 + blockHeight / 2);
                        break;
                    default:
                        framebuffer[row * Columns + column] = averageRegion(sourceX0, sourceY0, blockWidth, blockHeight);
                        break;
                    }
                }
            }

            const auto indicatorColor = [](const std::array<float, 3>& color) -> Color32
            {
                return {
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f))
                };
            };

            // Built-in shaders consume the same uniforms, and this final logical-LED override keeps
            // the indicator colors exact even when the shader surface is supersampled/downsampled.
            if (settings.ShaderKeyStateUniforms && settings.ShaderCapsLockColorEnabled && reactiveKeys.CapsLockActive)
                framebuffer[3 * Columns + 0] = indicatorColor(settings.ShaderCapsLockColor);
            if (settings.ShaderKeyStateUniforms && settings.ShaderScrollLockColorEnabled && reactiveKeys.ScrollLockActive)
                framebuffer[0 * Columns + 14] = indicatorColor(settings.ShaderScrollLockColor);

            glBindVertexArray(static_cast<GLuint>(previousVao));
            glUseProgram(static_cast<GLuint>(previousProgram));
            if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            if (ditherEnabled) glEnable(GL_DITHER); else glDisable(GL_DITHER);
            if (framebufferSrgbEnabled) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
            glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
            glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
            glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
            return true;
        }

        void shutdown() noexcept
        {
            if (_program != 0) glDeleteProgram(_program);
            if (_vao != 0) glDeleteVertexArrays(1, &_vao);
            if (_texture != 0) glDeleteTextures(1, &_texture);
            if (_framebuffer != 0) glDeleteFramebuffers(1, &_framebuffer);
            _program = 0;
            _vao = 0;
            _texture = 0;
            _framebuffer = 0;
            _width = 0;
            _height = 0;
            _pixels.clear();
        }

        bool ready() const noexcept { return _program != 0; }
        int width() const noexcept { return _width; }
        int height() const noexcept { return _height; }
        const std::string& status() const noexcept { return _status; }
        std::vector<ShaderMaterialParameter>& materialParameters() noexcept { return _materialParameters; }
        const std::vector<ShaderMaterialParameter>& materialParameters() const noexcept { return _materialParameters; }
        std::uint64_t materialRevision() const noexcept { return _materialRevision; }
        void markMaterialChanged() noexcept { ++_materialRevision; }
        void snapshotMaterialValues() { stashMaterialValues(); }
        bool saveMaterialValues()
        {
            stashMaterialValues();
            return saveShaderMaterialValueCache();
        }

        bool setMaterialParameter(const std::string_view id, const int component, const float value) noexcept
        {
            for (auto& parameter : _materialParameters)
            {
                if (parameter.PersistenceKey != id && parameter.Name != id) continue;
                const int index = std::clamp(component, 0, std::max(parameter.Components - 1, 0));
                if (parameter.Integer || parameter.Boolean)
                    parameter.IntValue[static_cast<std::size_t>(index)] = parameter.Boolean ? (value >= 0.5f ? 1 : 0) : static_cast<int>(std::lround(value));
                else
                    parameter.FloatValue[static_cast<std::size_t>(index)] = value;
                return true;
            }
            return false;
        }

        const ShaderMaterialParameter* findMaterialParameter(const std::string_view id) const noexcept
        {
            for (const auto& parameter : _materialParameters)
                if (parameter.PersistenceKey == id || parameter.Name == id) return &parameter;
            return nullptr;
        }

    private:
        void stashMaterialValues()
        {
            for (const auto& parameter : _materialParameters)
            {
                g_ShaderMaterialValues[parameter.PersistenceKey] = parameter.Integer || parameter.Boolean
                    ? shaderMaterialSerializeInts(parameter.IntValue, parameter.Components)
                    : shaderMaterialSerializeFloats(parameter.FloatValue, parameter.Components);
            }
        }

        void reflectMaterialParameters(const std::string_view vertexSource, const std::string_view fragmentSource)
        {
            _materialParameters.clear();
            if (_program == 0) return;
            const auto metadata = parseShaderUniformMetadata(vertexSource, fragmentSource);
            GLint count = 0;
            GLint maxNameLength = 0;
            glGetProgramiv(_program, GL_ACTIVE_UNIFORMS, &count);
            glGetProgramiv(_program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
            std::vector<char> nameBuffer(static_cast<std::size_t>(std::max(maxNameLength, 2)), '\0');
            for (GLint index = 0; index < count; ++index)
            {
                GLsizei nameLength = 0;
                GLint arraySize = 0;
                GLenum type = 0;
                glGetActiveUniform(_program, static_cast<GLuint>(index), static_cast<GLsizei>(nameBuffer.size()), &nameLength, &arraySize, &type, nameBuffer.data());
                if (nameLength <= 0 || arraySize != 1) continue;
                std::string name(nameBuffer.data(), static_cast<std::size_t>(nameLength));
                if (name.ends_with("[0]")) name.resize(name.size() - 3);
                const auto metadataIt = metadata.find(name);
                const ShaderUniformMetadata* ui = metadataIt == metadata.end() ? nullptr : &metadataIt->second;
                if (ui && ui->Hidden) continue;
                if (isEngineShaderUniform(name) && (!ui || !ui->Explicit)) continue;

                const int components = shaderUniformComponents(type);
                if (components == 0) continue;
                const GLint location = glGetUniformLocation(_program, name.c_str());
                if (location < 0) continue;

                ShaderMaterialParameter parameter{};
                parameter.Name = name;
                parameter.Label = ui && !ui->Label.empty() ? ui->Label : prettyUniformLabel(name);
                parameter.PersistenceKey = ui && !ui->Id.empty() ? ui->Id : name;
                parameter.Type = type;
                parameter.Location = location;
                parameter.Components = components;
                parameter.Integer = type == GL_INT || type == GL_INT_VEC2 || type == GL_INT_VEC3 || type == GL_INT_VEC4;
                parameter.Boolean = type == GL_BOOL || type == GL_BOOL_VEC2 || type == GL_BOOL_VEC3 || type == GL_BOOL_VEC4;
                parameter.Color = ui && ui->Color && !parameter.Integer && !parameter.Boolean && (components == 3 || components == 4);
                parameter.HasMin = ui && ui->HasMin;
                parameter.HasMax = ui && ui->HasMax;
                parameter.Min = ui ? ui->Min : 0.0f;
                parameter.Max = ui ? ui->Max : 1.0f;
                parameter.Step = ui && ui->HasStep ? std::max(ui->Step, 0.000001f) : (parameter.Integer || parameter.Boolean ? 1.0f : 0.01f);

                if (parameter.Integer || parameter.Boolean)
                {
                    glGetUniformiv(_program, location, parameter.IntValue.data());
                    parameter.IntDefault = parameter.IntValue;
                    if (ui && ui->HasDefault)
                    {
                        for (int component = 0; component < components; ++component)
                            parameter.IntValue[static_cast<std::size_t>(component)] = parameter.IntDefault[static_cast<std::size_t>(component)] = static_cast<int>(std::lround(ui->Default[static_cast<std::size_t>(component)]));
                    }
                    if (const auto saved = g_ShaderMaterialValues.find(parameter.PersistenceKey); saved != g_ShaderMaterialValues.end())
                    {
                        std::array<int, 4> values{};
                        if (parseShaderMaterialInts(saved->second, values, components)) parameter.IntValue = values;
                    }
                }
                else
                {
                    glGetUniformfv(_program, location, parameter.FloatValue.data());
                    parameter.FloatDefault = parameter.FloatValue;
                    if (ui && ui->HasDefault)
                    {
                        for (int component = 0; component < components; ++component)
                            parameter.FloatValue[static_cast<std::size_t>(component)] = parameter.FloatDefault[static_cast<std::size_t>(component)] = ui->Default[static_cast<std::size_t>(component)];
                    }
                    if (const auto saved = g_ShaderMaterialValues.find(parameter.PersistenceKey); saved != g_ShaderMaterialValues.end())
                    {
                        std::array<float, 4> values{};
                        if (parseShaderMaterialFloats(saved->second, values, components)) parameter.FloatValue = values;
                    }
                }
                _materialParameters.push_back(std::move(parameter));
            }
            std::sort(_materialParameters.begin(), _materialParameters.end(), [](const auto& a, const auto& b) { return a.Label < b.Label; });
        }

        void applyMaterialParameters() const noexcept
        {
            for (const auto& parameter : _materialParameters)
            {
                if (parameter.Location < 0) continue;
                if (parameter.Boolean || parameter.Integer)
                {
                    switch (parameter.Components)
                    {
                    case 1: glUniform1i(parameter.Location, parameter.IntValue[0]); break;
                    case 2: glUniform2iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    case 3: glUniform3iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    case 4: glUniform4iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    default: break;
                    }
                }
                else
                {
                    switch (parameter.Components)
                    {
                    case 1: glUniform1f(parameter.Location, parameter.FloatValue[0]); break;
                    case 2: glUniform2fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    case 3: glUniform3fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    case 4: glUniform4fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    default: break;
                    }
                }
            }
        }

        GLuint compileStage(const GLenum type, const std::string_view source)
        {
            const GLuint shader = glCreateShader(type);
            const char* data = source.data();
            const GLint length = static_cast<GLint>(source.size());
            glShaderSource(shader, 1, &data, &length);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE)
                return shader;
            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
            glGetShaderInfoLog(shader, logLength, nullptr, log.data());
            _status = (type == GL_VERTEX_SHADER ? "Vertex compile error:\n" : "Fragment compile error:\n") + log;
            glDeleteShader(shader);
            return 0;
        }

        void uniform1f(const char* name, const float value) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform1f(location, value);
        }
        void uniform1i(const char* name, const int value) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform1i(location, value);
        }
        void uniform2f(const char* name, const float x, const float y) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform2f(location, x, y);
        }
        void uniform3f(const char* name, const float x, const float y, const float z) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform3f(location, x, y, z);
        }

        GLuint _framebuffer = 0;
        GLuint _texture = 0;
        GLuint _vao = 0;
        GLuint _program = 0;
        int _width = 0;
        int _height = 0;
        std::vector<std::uint8_t> _pixels;
        std::vector<ShaderMaterialParameter> _materialParameters;
        std::uint64_t _materialRevision = 0;
        std::string _status = "Shader not compiled";
    };

    static void applyShaderDiagnostics(TextEditor& editor, const std::string_view log)
    {
        editor.ClearMarkers();
        static const std::regex colonPattern(R"((?:ERROR|WARNING)?\s*:?\s*\d+:(\d+)(?::|\())", std::regex::icase);
        static const std::regex parenPattern(R"(\d+\((\d+)\))");
        std::size_t start = 0;
        while (start < log.size())
        {
            const std::size_t end = log.find('\n', start);
            std::string line(log.substr(start, end == std::string_view::npos ? log.size() - start : end - start));
            std::smatch match;
            std::size_t lineNumber = 0;
            if (std::regex_search(line, match, colonPattern) || std::regex_search(line, match, parenPattern))
            {
                try { lineNumber = static_cast<std::size_t>(std::stoul(match[1].str())); } catch (...) { lineNumber = 0; }
            }
            if (lineNumber > 0)
            {
                const bool warning = line.find("warning") != std::string::npos || line.find("WARNING") != std::string::npos;
                const ImU32 numberColor = warning ? IM_COL32(255, 195, 70, 255) : IM_COL32(255, 85, 85, 255);
                const ImU32 textColor = warning ? IM_COL32(115, 80, 10, 55) : IM_COL32(120, 20, 20, 65);
                editor.AddMarker(lineNumber - 1, numberColor, textColor, line, line);
            }
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    static void updateShaderDiagnostics(ShaderEditorState& editors, const std::string& status)
    {
        if (!editors.Initialized)
            return;
        editors.Vertex.ClearMarkers();
        editors.Fragment.ClearMarkers();
        if (status.starts_with("Vertex compile error:"))
            applyShaderDiagnostics(editors.Vertex, status);
        else if (status.starts_with("Fragment compile error:"))
            applyShaderDiagnostics(editors.Fragment, status);
    }

    static bool compileShaders(ShaderFramebuffer& framebuffer, ShaderEditorState& editors, const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        const bool result = framebuffer.compile(vertexSource.data(), fragmentSource.data());
        updateShaderDiagnostics(editors, framebuffer.status());
        return result;
    }

    struct ShaderTransitionState
    {
        ShaderFramebuffer Previous;
        std::array<Color32, MatrixSize> Frame{};
        bool Active = false;
        double StartedAt = 0.0;
        float Duration = 0.0f;

        void cancel() noexcept
        {
            Active = false;
            StartedAt = 0.0;
            Duration = 0.0f;
            Previous.shutdown();
        }
    };

    static Color32 lerpColorLinear(const Color32 from, const Color32 to, const float amount) noexcept
    {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        const auto mix = [t](const std::uint8_t a, const std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(static_cast<float>(a) + (static_cast<float>(b) - a) * t)); };
        return {mix(from.R, to.R), mix(from.G, to.G), mix(from.B, to.B)};
    }

    static bool switchShaderPreset(ShaderFramebuffer& framebuffer, ShaderTransitionState& transition, ShaderEditorState& editors, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const int presetIndex, const double now, const float transitionSeconds, const bool persist = true)
    {
        if (presetIndex <= 0 || presetIndex > static_cast<int>(ShaderPresets.size())) return false;
        const auto& preset = ShaderPresets[static_cast<std::size_t>(presetIndex - 1)];
        if (std::string_view(fragmentSource.data()) == preset.FragmentSource)
        {
            settings.ShaderPresetIndex = presetIndex;
            return true;
        }

        const std::array<char, ShaderSourceCapacity> previousSource = fragmentSource;
        transition.cancel();
        const float duration = std::clamp(transitionSeconds, 0.0f, 10.0f);
        if (duration > 0.0f && framebuffer.ready())
        {
            framebuffer.snapshotMaterialValues();
            if (transition.Previous.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight) && transition.Previous.compile(vertexSource.data(), previousSource.data()))
            {
                transition.Active = true;
                transition.StartedAt = now;
                transition.Duration = duration;
            }
            else
                transition.cancel();
        }

        setShaderSource(fragmentSource, preset.FragmentSource);
        if (!compileShaders(framebuffer, editors, vertexSource, fragmentSource))
        {
            fragmentSource = previousSource;
            transition.cancel();
            return false;
        }

        settings.ShaderPresetIndex = presetIndex;
        settings.BaseColorMode = 2;
        if (editors.Initialized) editors.Fragment.SetText(fragmentSource.data());
        if (persist) saveShaderSources(vertexSource, fragmentSource);
        return true;
    }

    struct PacketBuffer
    {
        alignas(4) std::array<std::byte, 512> Data{};
        std::size_t Size = 0;
    };

    template<typename T>
    static PacketBuffer makePacket(const PacketType type, const T& payload)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader) + sizeof(T);
        auto* header = std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, sizeof(T));
        std::memcpy(packet.Data.data() + sizeof(PacketHeader), &payload, sizeof(T));
        return packet;
    }

    static PacketBuffer makePacket(const PacketType type)
    {
        PacketBuffer packet{};
        packet.Size = sizeof(PacketHeader);
        std::construct_at(reinterpret_cast<PacketHeader*>(packet.Data.data()), 1, type, PacketDirection::HostToDevice, 0, 0);
        return packet;
    }

    struct USBStatsSnapshot
    {
        std::uint64_t TxBytes = 0;
        std::uint64_t RxBytes = 0;
        std::uint64_t TxTransfers = 0;
        std::uint64_t RxTransfers = 0;
        std::uint64_t TxErrors = 0;
        std::uint64_t RxErrors = 0;
        std::uint64_t Connects = 0;
        std::uint64_t Disconnects = 0;
        double LastTxMilliseconds = 0.0;
        double LastRxMilliseconds = 0.0;
    };

    class RawUSB
    {
    public:
        using PacketHandler = std::function<void(const PacketHeader&)>;
        using PacketObserver = std::function<void(bool, const PacketHeader&, std::span<const std::byte>)>;

        ~RawUSB()
        {
            shutdown();
        }

        bool initialize() noexcept
        {
            if (_context)
                return true;
            const int result = libusb_init(&_context);
            _lastError.store(result, std::memory_order_release);
            return result == LIBUSB_SUCCESS;
        }

        bool connect() noexcept
        {
            if (_connected.load(std::memory_order_acquire))
                return true;
            if (!_context && !initialize())
                return false;
            if (_handle)
                disconnect();

            _handle = libusb_open_device_with_vid_pid(_context, VendorId, ProductId);
            if (!_handle)
            {
                _lastError.store(LIBUSB_ERROR_NO_DEVICE, std::memory_order_release);
                return false;
            }

            _deviceName.clear();
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(libusb_get_device(_handle), &descriptor) == LIBUSB_SUCCESS && descriptor.iProduct != 0)
            {
                std::array<unsigned char, 256> product{};
                const int length = libusb_get_string_descriptor_ascii(_handle, descriptor.iProduct, product.data(), static_cast<int>(product.size() - 1));
                if (length > 0)
                    _deviceName.assign(reinterpret_cast<const char*>(product.data()), static_cast<std::size_t>(length));
            }
            if (_deviceName.empty())
                _deviceName = "Quartz K552X";

            const int detachResult = libusb_set_auto_detach_kernel_driver(_handle, 1);
            if (detachResult != LIBUSB_SUCCESS && detachResult != LIBUSB_ERROR_NOT_SUPPORTED)
            {
                _lastError.store(detachResult, std::memory_order_release);
                libusb_close(_handle);
                _handle = nullptr;
                return false;
            }

            const int claimResult = libusb_claim_interface(_handle, RPCInterface);
            if (claimResult != LIBUSB_SUCCESS)
            {
                _lastError.store(claimResult, std::memory_order_release);
                libusb_close(_handle);
                _handle = nullptr;
                return false;
            }

            _running.store(true, std::memory_order_release);
            _connected.store(true, std::memory_order_release);
            _lastError.store(LIBUSB_SUCCESS, std::memory_order_release);
            _connects.fetch_add(1, std::memory_order_relaxed);
            try
            {
                _receiveThread = std::thread(&RawUSB::_receiveLoop, this);
            }
            catch (...)
            {
                _running.store(false, std::memory_order_release);
                _connected.store(false, std::memory_order_release);
                libusb_release_interface(_handle, RPCInterface);
                libusb_close(_handle);
                _handle = nullptr;
                _lastError.store(LIBUSB_ERROR_OTHER, std::memory_order_release);
                return false;
            }
            return true;
        }

        void disconnect() noexcept
        {
            const bool hadDevice = _handle != nullptr || _connected.load(std::memory_order_acquire);
            _running.store(false, std::memory_order_release);
            _connected.store(false, std::memory_order_release);
            if (_receiveThread.joinable())
                _receiveThread.join();
            if (_handle)
            {
                libusb_release_interface(_handle, RPCInterface);
                libusb_close(_handle);
                _handle = nullptr;
            }
            _deviceName.clear();
            _rxAssembly.clear();
            if (hadDevice) _disconnects.fetch_add(1, std::memory_order_relaxed);
        }

        void shutdown() noexcept
        {
            disconnect();
            if (_context)
            {
                libusb_exit(_context);
                _context = nullptr;
            }
        }

        bool isConnected() const noexcept
        {
            return _connected.load(std::memory_order_acquire);
        }

        int lastError() const noexcept
        {
            return _lastError.load(std::memory_order_acquire);
        }

        const std::string& deviceName() const noexcept
        {
            return _deviceName;
        }

        void setPacketHandler(PacketHandler handler)
        {
            _packetHandler = std::move(handler);
        }

        void setPacketObserver(PacketObserver observer)
        {
            _packetObserver = std::move(observer);
        }

        USBStatsSnapshot stats() const noexcept
        {
            return {
                _txBytes.load(std::memory_order_relaxed),
                _rxBytes.load(std::memory_order_relaxed),
                _txTransfers.load(std::memory_order_relaxed),
                _rxTransfers.load(std::memory_order_relaxed),
                _txErrors.load(std::memory_order_relaxed),
                _rxErrors.load(std::memory_order_relaxed),
                _connects.load(std::memory_order_relaxed),
                _disconnects.load(std::memory_order_relaxed),
                _lastTxMicroseconds.load(std::memory_order_relaxed) / 1000.0,
                _lastRxMicroseconds.load(std::memory_order_relaxed) / 1000.0
            };
        }

        bool send(const PacketBuffer& packet) noexcept
        {
            if (!_handle || !_connected.load(std::memory_order_acquire) || packet.Size == 0 || packet.Size > packet.Data.size())
                return false;
            int transferred = 0;
            const auto transferStart = std::chrono::steady_clock::now();
            const int result = libusb_bulk_transfer(_handle, RequestEndpoint, reinterpret_cast<unsigned char*>(const_cast<std::byte*>(packet.Data.data())), static_cast<int>(packet.Size), &transferred, 20);
            const auto transferEnd = std::chrono::steady_clock::now();
            _lastTxMicroseconds.store(std::chrono::duration<double, std::micro>(transferEnd - transferStart).count(), std::memory_order_relaxed);
            _txTransfers.fetch_add(1, std::memory_order_relaxed);
            if (transferred > 0) _txBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
            if (result != LIBUSB_SUCCESS || transferred != static_cast<int>(packet.Size)) _txErrors.fetch_add(1, std::memory_order_relaxed);
            _lastError.store(result, std::memory_order_release);
            if (result == LIBUSB_ERROR_NO_DEVICE)
            {
                _running.store(false, std::memory_order_release);
                _connected.store(false, std::memory_order_release);
            }
            const bool success = result == LIBUSB_SUCCESS && transferred == static_cast<int>(packet.Size);
            if (success && _packetObserver)
            {
                const std::span<const std::byte> bytes(packet.Data.data(), packet.Size);
                if (const auto* header = PacketHeader::asPtr(bytes))
                    _packetObserver(true, *header, bytes);
            }
            return success;
        }

    private:
        static constexpr int RPCInterface = RPCInterfaceNumber;
        static constexpr unsigned char RequestEndpoint = RPCOutEndpoint;
        static constexpr unsigned char ResponseEndpoint = RPCInEndpoint;

        void _receiveLoop() noexcept
        {
            std::array<unsigned char, 512> buffer{};
            while (_running.load(std::memory_order_acquire))
            {
                int transferred = 0;
                const auto transferStart = std::chrono::steady_clock::now();
                const int result = libusb_bulk_transfer(_handle, ResponseEndpoint, buffer.data(), static_cast<int>(buffer.size()), &transferred, 100);
                const auto transferEnd = std::chrono::steady_clock::now();
                if (result == LIBUSB_ERROR_TIMEOUT)
                    continue;
                _lastRxMicroseconds.store(std::chrono::duration<double, std::micro>(transferEnd - transferStart).count(), std::memory_order_relaxed);
                _rxTransfers.fetch_add(1, std::memory_order_relaxed);
                if (transferred > 0) _rxBytes.fetch_add(static_cast<std::uint64_t>(transferred), std::memory_order_relaxed);
                _lastError.store(result, std::memory_order_release);
                if (result != LIBUSB_SUCCESS)
                {
                    _rxErrors.fetch_add(1, std::memory_order_relaxed);
                    if (result == LIBUSB_ERROR_NO_DEVICE)
                        _connected.store(false, std::memory_order_release);
                    break;
                }
                if (transferred > 0)
                    _consumeReceivedBytes(reinterpret_cast<const std::byte*>(buffer.data()), static_cast<std::size_t>(transferred));
            }
            _running.store(false, std::memory_order_release);
        }

        void _consumeReceivedBytes(const std::byte* data, const std::size_t size)
        {
            _rxAssembly.insert(_rxAssembly.end(), data, data + size);
            while (_rxAssembly.size() >= sizeof(PacketHeader))
            {
                std::uint32_t magic = 0;
                std::memcpy(&magic, _rxAssembly.data(), sizeof(magic));
                if (magic != PacketHeader::MAGIC_NUMBER)
                {
                    _rxAssembly.erase(_rxAssembly.begin());
                    continue;
                }

                PacketBuffer packet{};
                std::memcpy(packet.Data.data(), _rxAssembly.data(), sizeof(PacketHeader));
                const auto* header = reinterpret_cast<const PacketHeader*>(packet.Data.data());
                const std::size_t packetSize = sizeof(PacketHeader) + header->PayloadLength;
                if (packetSize > packet.Data.size())
                {
                    _rxAssembly.clear();
                    return;
                }
                if (_rxAssembly.size() < packetSize)
                    return;

                packet.Size = packetSize;
                std::memcpy(packet.Data.data(), _rxAssembly.data(), packetSize);
                header = PacketHeader::asPtr(std::span<const std::byte>(packet.Data.data(), packet.Size));
                if (header && _packetObserver)
                    _packetObserver(false, *header, std::span<const std::byte>(packet.Data.data(), packet.Size));
                if (header && _packetHandler)
                    _packetHandler(*header);
                _rxAssembly.erase(_rxAssembly.begin(), _rxAssembly.begin() + static_cast<std::ptrdiff_t>(packetSize));
            }
        }

        libusb_context* _context = nullptr;
        libusb_device_handle* _handle = nullptr;
        std::thread _receiveThread;
        std::atomic_bool _running = false;
        std::atomic_bool _connected = false;
        std::atomic_int _lastError = LIBUSB_SUCCESS;
        std::atomic_uint64_t _txBytes{0}, _rxBytes{0}, _txTransfers{0}, _rxTransfers{0}, _txErrors{0}, _rxErrors{0}, _connects{0}, _disconnects{0};
        std::atomic<double> _lastTxMicroseconds{0.0}, _lastRxMicroseconds{0.0};
        PacketHandler _packetHandler;
        PacketObserver _packetObserver;
        std::vector<std::byte> _rxAssembly;
        std::string _deviceName;
    };

    struct AudioLevelSnapshot
    {
        float Rms = 0.0f;
        float Peak = 0.0f;
    };

    class AudioSpectrum
    {
    public:
        static constexpr int SampleRate = 48000;
        static constexpr int Channels = 2;
        static constexpr int BytesPerSample = sizeof(float);
        static constexpr int BytesPerFrame = BytesPerSample * Channels;

        ~AudioSpectrum() { stop(); }

        bool start(const std::string& source)
        {
            stop();
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
            {
                _error = "pipe() failed";
                return false;
            }
            const std::string deviceArg = "--device=" + source;
            const pid_t pid = fork();
            if (pid < 0)
            {
                close(pipeFds[0]);
                close(pipeFds[1]);
                _error = "fork() failed";
                return false;
            }
            if (pid == 0)
            {
                dup2(pipeFds[1], STDOUT_FILENO);
                close(pipeFds[0]);
                close(pipeFds[1]);
                setenv("PULSE_LATENCY_MSEC", "5", 1);
                execlp("parec", "parec", deviceArg.c_str(), "--format=float32le", "--rate=48000", "--channels=2", "--raw", static_cast<char*>(nullptr));
                _exit(127);
            }
            close(pipeFds[1]);
            _readFd = pipeFds[0];
            _pid = pid;
            _source = source;
            _error.clear();
            _running.store(true, std::memory_order_release);
            _thread = std::thread(&AudioSpectrum::readLoop, this);
            return true;
        }

        void stop()
        {
            if (_pid > 0)
                kill(_pid, SIGTERM);
            _running.store(false, std::memory_order_release);
            if (_thread.joinable())
                _thread.join();
            if (_readFd >= 0)
            {
                close(_readFd);
                _readFd = -1;
            }
            if (_pid > 0)
            {
                int status = 0;
                waitpid(_pid, &status, 0);
                _pid = -1;
            }
        }

        bool isRunning() const noexcept { return _running.load(std::memory_order_acquire); }
        const std::string& source() const noexcept { return _source; }
        const std::string& error() const noexcept { return _error; }

        AudioLevelSnapshot levelSnapshot()
        {
            AudioLevelSnapshot result{};
            std::lock_guard lock(_sampleMutex);
            const int available = std::min(_sampleCount, static_cast<int>(FFTSize));
            if (available <= 0) return result;
            const int start = (_writePosition - available + static_cast<int>(FFTSize)) % static_cast<int>(FFTSize);
            double sumSquares = 0.0;
            for (int i = 0; i < available; ++i)
            {
                const float sample = _samples[(start + i) % static_cast<int>(FFTSize)];
                sumSquares += static_cast<double>(sample) * sample;
                result.Peak = std::max(result.Peak, std::abs(sample));
            }
            result.Rms = static_cast<float>(std::sqrt(sumSquares / available));
            return result;
        }

        void getBands(const std::span<float> bands, const float minFrequency, const float maxFrequency, const float minDb, const float maxDb)
        {
            std::array<float, FFTSize> samples{};
            {
                std::lock_guard lock(_sampleMutex);
                const int available = std::min(_sampleCount, static_cast<int>(FFTSize));
                const int padding = static_cast<int>(FFTSize) - available;
                const int start = (_writePosition - available + static_cast<int>(FFTSize)) % static_cast<int>(FFTSize);
                for (int i = 0; i < available; ++i)
                    samples[padding + i] = _samples[(start + i) % static_cast<int>(FFTSize)];
            }

            std::array<std::complex<float>, FFTSize> fft{};
            for (std::size_t i = 0; i < FFTSize; ++i)
            {
                const float window = 0.5f - 0.5f * std::cos(2.0f * Pi * static_cast<float>(i) / static_cast<float>(FFTSize - 1));
                fft[i] = {samples[i] * window, 0.0f};
            }
            transform(fft);

            const float safeMinFrequency = std::clamp(minFrequency, 1.0f, SampleRate * 0.49f);
            const float safeMaxFrequency = std::clamp(maxFrequency, safeMinFrequency + 1.0f, SampleRate * 0.49f);
            const float safeMinDb = std::min(minDb, maxDb - 0.1f);
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                const float normalizedLow = static_cast<float>(band) / static_cast<float>(bands.size());
                const float normalizedHigh = static_cast<float>(band + 1) / static_cast<float>(bands.size());
                const float ratio = safeMaxFrequency / safeMinFrequency;
                const float lowFrequency = safeMinFrequency * std::pow(ratio, normalizedLow);
                const float highFrequency = safeMinFrequency * std::pow(ratio, normalizedHigh);
                const int lowBin = std::clamp(static_cast<int>(std::floor(lowFrequency * FFTSize / SampleRate)), 1, static_cast<int>(FFTSize / 2 - 1));
                const int highBin = std::clamp(static_cast<int>(std::ceil(highFrequency * FFTSize / SampleRate)), lowBin, static_cast<int>(FFTSize / 2 - 1));
                float maxMagnitude = 0.0f;
                for (int bin = lowBin; bin <= highBin; ++bin)
                    maxMagnitude = std::max(maxMagnitude, std::abs(fft[bin]) * 4.0f / static_cast<float>(FFTSize));
                const float db = 20.0f * std::log10(maxMagnitude + 1e-9f);
                bands[band] = std::clamp((db - safeMinDb) / (maxDb - safeMinDb), 0.0f, 1.0f);
            }
        }

    private:
        static void transform(std::array<std::complex<float>, FFTSize>& values)
        {
            for (std::size_t i = 1, j = 0; i < FFTSize; ++i)
            {
                std::size_t bit = FFTSize >> 1;
                while ((j & bit) != 0)
                {
                    j ^= bit;
                    bit >>= 1;
                }
                j ^= bit;
                if (i < j)
                    std::swap(values[i], values[j]);
            }
            for (std::size_t length = 2; length <= FFTSize; length <<= 1)
            {
                const float angle = -2.0f * Pi / static_cast<float>(length);
                const std::complex<float> step(std::cos(angle), std::sin(angle));
                for (std::size_t offset = 0; offset < FFTSize; offset += length)
                {
                    std::complex<float> factor(1.0f, 0.0f);
                    const std::size_t half = length >> 1;
                    for (std::size_t i = 0; i < half; ++i)
                    {
                        const auto even = values[offset + i];
                        const auto odd = values[offset + i + half] * factor;
                        values[offset + i] = even + odd;
                        values[offset + i + half] = even - odd;
                        factor *= step;
                    }
                }
            }
        }

        void readLoop()
        {
            std::array<std::byte, 8192 + BytesPerFrame> buffer{};
            std::size_t carry = 0;
            while (_running.load(std::memory_order_acquire))
            {
                const ssize_t bytesRead = read(_readFd, buffer.data() + carry, buffer.size() - carry);
                if (bytesRead <= 0)
                    break;
                const std::size_t totalBytes = carry + static_cast<std::size_t>(bytesRead);
                const std::size_t frameCount = totalBytes / BytesPerFrame;
                const std::size_t consumedBytes = frameCount * BytesPerFrame;
                {
                    std::lock_guard lock(_sampleMutex);
                    for (std::size_t frame = 0; frame < frameCount; ++frame)
                    {
                        const std::size_t offset = frame * BytesPerFrame;
                        float left;
                        float right;
                        std::memcpy(&left, buffer.data() + offset, sizeof(float));
                        std::memcpy(&right, buffer.data() + offset + sizeof(float), sizeof(float));
                        _samples[_writePosition] = (left + right) * 0.5f;
                        _writePosition = (_writePosition + 1) % static_cast<int>(FFTSize);
                        if (_sampleCount < static_cast<int>(FFTSize))
                            ++_sampleCount;
                    }
                }
                carry = totalBytes - consumedBytes;
                if (carry != 0)
                    std::memmove(buffer.data(), buffer.data() + consumedBytes, carry);
            }
            _running.store(false, std::memory_order_release);
        }

        std::array<float, FFTSize> _samples{};
        std::mutex _sampleMutex;
        std::thread _thread;
        std::atomic_bool _running = false;
        pid_t _pid = -1;
        int _readFd = -1;
        int _writePosition = 0;
        int _sampleCount = 0;
        std::string _source;
        std::string _error;
    };

    struct AutoGainState
    {
        float LongTermRms = 0.0f;
        float Correction = 1.0f;
        float EffectiveGain = 1.62f;
        float SilenceSeconds = 0.0f;
        bool Initialized = false;

        void reset(const VisualizerSettings& settings) noexcept
        {
            LongTermRms = 0.0f;
            Correction = 1.0f;
            EffectiveGain = settings.AutomaticOverallGain ? settings.AutoGainBaseline : settings.OverallGain;
            SilenceSeconds = 0.0f;
            Initialized = false;
        }

        void update(const AudioLevelSnapshot& level, const VisualizerSettings& settings, const float dt) noexcept
        {
            if (!settings.AutomaticOverallGain)
            {
                Correction = 1.0f;
                EffectiveGain = settings.OverallGain;
                SilenceSeconds = 0.0f;
                return;
            }

            const float safeDt = std::clamp(dt, 0.0001f, 0.25f);
            if (level.Rms > settings.AutoGainSilenceGate)
            {
                SilenceSeconds = 0.0f;
                const float loudnessAlpha = 1.0f - std::exp(-settings.AutoGainAdaptation * safeDt);
                if (!Initialized)
                {
                    LongTermRms = level.Rms;
                    Initialized = true;
                }
                else
                    LongTermRms += (level.Rms - LongTermRms) * loudnessAlpha;

                const float desired = std::clamp(settings.AutoGainTargetRms / std::max(LongTermRms, 0.000001f), settings.AutoGainMinCorrection, settings.AutoGainMaxCorrection);
                const float correctionAlpha = 1.0f - std::exp(-std::max(settings.AutoGainAdaptation * 0.55f, 0.02f) * safeDt);
                Correction += (desired - Correction) * correctionAlpha;
            }
            else
            {
                SilenceSeconds += safeDt;
                if (SilenceSeconds > 3.0f)
                {
                    const float returnAlpha = 1.0f - std::exp(-0.12f * safeDt);
                    Correction += (1.0f - Correction) * returnAlpha;
                }
            }
            Correction = std::clamp(Correction, settings.AutoGainMinCorrection, settings.AutoGainMaxCorrection);
            EffectiveGain = settings.AutoGainBaseline * Correction;
        }
    };

    static std::string shellQuote(const std::string_view value)
    {
        std::string result = "'";
        for (const char c : value)
        {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        result += '\'';
        return result;
    }

    static std::vector<std::byte> readCommand(const std::string& command, const std::size_t maxBytes = 16 * 1024 * 1024)
    {
        std::vector<std::byte> result;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return result;
        std::array<std::byte, 8192> buffer{};
        while (result.size() < maxBytes)
        {
            const std::size_t readSize = std::fread(buffer.data(), 1, std::min(buffer.size(), maxBytes - result.size()), pipe);
            if (readSize == 0)
                break;
            result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(readSize));
        }
        pclose(pipe);
        return result;
    }

    static std::string bytesToString(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }


    static std::string trim(std::string value)
    {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        std::size_t start = 0;
        while (start < value.size() && (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t'))
            ++start;
        if (start != 0)
            value.erase(0, start);
        return value;
    }

    static bool commandExists(const std::string_view command)
    {
        return !trim(bytesToString(readCommand("command -v " + shellQuote(command) + " 2>/dev/null", 4096))).empty();
    }

    struct AudioSourceInfo
    {
        std::string Name;
        std::string Description;
    };

    static std::vector<AudioSourceInfo> enumerateAudioSources()
    {
        std::vector<AudioSourceInfo> sources;
        if (!commandExists("pactl"))
            return sources;
        const std::string output = bytesToString(readCommand("pactl list sources 2>/dev/null", 512 * 1024));
        AudioSourceInfo current;
        const auto flush = [&]
        {
            if (current.Name.empty())
                return;
            if (current.Description.empty())
                current.Description = current.Name;
            if (std::ranges::none_of(sources, [&](const auto& source) { return source.Name == current.Name; }))
                sources.emplace_back(std::move(current));
            current = {};
        };
        std::size_t start = 0;
        while (start < output.size())
        {
            const std::size_t end = output.find('\n', start);
            const std::string line = trim(std::string(output.substr(start, end == std::string::npos ? output.size() - start : end - start)));
            if (line.starts_with("Source #"))
                flush();
            else if (line.starts_with("Name:"))
                current.Name = trim(line.substr(5));
            else if (line.starts_with("Description:"))
                current.Description = trim(line.substr(12));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        flush();
        std::ranges::sort(sources, {}, &AudioSourceInfo::Description);
        return sources;
    }

    static std::vector<std::string> split(const std::string_view value, const char delimiter)
    {
        std::vector<std::string> result;
        std::size_t start = 0;
        for (;;)
        {
            const std::size_t end = value.find(delimiter, start);
            result.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return result;
    }

    static std::string percentDecode(const std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const auto hex = [](const char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int high = hex(value[i + 1]);
                const int low = hex(value[i + 2]);
                if (high >= 0 && low >= 0)
                {
                    result += static_cast<char>((high << 4) | low);
                    i += 2;
                    continue;
                }
            }
            result += value[i];
        }
        return result;
    }

    class MediaColorProvider
    {
    public:
        ~MediaColorProvider() { stop(); }

        void start()
        {
            if (_running.exchange(true, std::memory_order_acq_rel))
                return;
            _thread = std::thread(&MediaColorProvider::run, this);
        }

        void stop()
        {
            _running.store(false, std::memory_order_release);
            if (_thread.joinable())
                _thread.join();
        }

        void setPollInterval(const float seconds) noexcept { _pollMilliseconds.store(static_cast<int>(std::clamp(seconds, 0.10f, 5.0f) * 1000.0f), std::memory_order_release); }

        std::optional<Color32> targetColor() const noexcept
        {
            const std::int32_t packed = _targetColor.load(std::memory_order_acquire);
            if (packed < 0)
                return std::nullopt;
            return Color32{
                static_cast<std::uint8_t>(packed >> 16),
                static_cast<std::uint8_t>(packed >> 8),
                static_cast<std::uint8_t>(packed)
            };
        }

        bool playing() const noexcept { return _playing.load(std::memory_order_acquire); }

        std::string mediaTitle() const
        {
            std::lock_guard lock(_stateMutex);
            return _mediaTitle;
        }

        std::string status() const
        {
            std::lock_guard lock(_stateMutex);
            return _status;
        }

        static constexpr bool imageDecoderAvailable() noexcept { return QUARTZ_HAS_STB_IMAGE != 0; }

    private:
        struct MediaInfo
        {
            std::string Player;
            std::string Status;
            std::string Title;
            std::string Artist;
            std::string ArtworkUrl;
        };

        struct MediaDiscovery
        {
            std::optional<MediaInfo> Media;
            std::string Status;
        };

        struct Bucket
        {
            std::uint64_t R = 0;
            std::uint64_t G = 0;
            std::uint64_t B = 0;
            std::uint32_t Count = 0;
            float Score = 0.0f;
        };

        static std::vector<std::string> tokenizeBusctl(const std::string_view value)
        {
            std::vector<std::string> tokens;
            std::size_t i = 0;
            while (i < value.size())
            {
                while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\r' || value[i] == '\n'))
                    ++i;
                if (i >= value.size())
                    break;
                std::string token;
                if (value[i] == '"')
                {
                    ++i;
                    while (i < value.size() && value[i] != '"')
                    {
                        if (value[i] == '\\' && i + 1 < value.size())
                        {
                            ++i;
                            switch (value[i])
                            {
                            case 'n': token += '\n'; break;
                            case 'r': token += '\r'; break;
                            case 't': token += '\t'; break;
                            default: token += value[i]; break;
                            }
                            ++i;
                            continue;
                        }
                        token += value[i++];
                    }
                    if (i < value.size() && value[i] == '"')
                        ++i;
                }
                else
                {
                    const std::size_t start = i;
                    while (i < value.size() && value[i] != ' ' && value[i] != '\t' && value[i] != '\r' && value[i] != '\n')
                        ++i;
                    token.assign(value.substr(start, i - start));
                }
                tokens.emplace_back(std::move(token));
            }
            return tokens;
        }

        static std::vector<std::string> mprisPlayers()
        {
            std::vector<std::string> players;
            const std::string output = bytesToString(readCommand("busctl --user --no-pager --no-legend --full list 2>/dev/null", 256 * 1024));
            std::size_t start = 0;
            while (start < output.size())
            {
                const std::size_t end = output.find('\n', start);
                const std::string line = trim(std::string(output.substr(start, end == std::string::npos ? output.size() - start : end - start)));
                if (!line.empty())
                {
                    const std::size_t space = line.find_first_of(" \t");
                    const std::string name = line.substr(0, space);
                    if (name.starts_with("org.mpris.MediaPlayer2."))
                        players.emplace_back(name);
                }
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return players;
        }

        static std::vector<std::string> mprisProperty(const std::string& player, const std::string_view property)
        {
            const std::string command = "busctl --user --no-pager get-property " + shellQuote(player) + " /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player " + shellQuote(property) + " 2>/dev/null";
            return tokenizeBusctl(bytesToString(readCommand(command, 256 * 1024)));
        }

        static std::string scalarProperty(const std::vector<std::string>& tokens)
        {
            return tokens.size() >= 2 ? tokens[1] : std::string{};
        }

        static std::string metadataString(const std::vector<std::string>& tokens, const std::string_view key)
        {
            for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
            {
                if (tokens[i] != key)
                    continue;
                if (tokens[i + 1] == "s" || tokens[i + 1] == "o")
                    return tokens[i + 2];
                if (tokens[i + 1] == "as" && i + 3 < tokens.size())
                    return tokens[i + 3];
                return {};
            }
            return {};
        }

        static std::string youtubeThumbnail(const std::string_view url)
        {
            std::string videoId;
            if (const std::size_t shortPos = url.find("youtu.be/"); shortPos != std::string_view::npos)
            {
                const std::size_t start = shortPos + 9;
                const std::size_t end = url.find_first_of("?#&/", start);
                videoId = std::string(url.substr(start, end == std::string_view::npos ? url.size() - start : end - start));
            }
            else if (url.find("youtube.com") != std::string_view::npos)
            {
                const std::size_t v = url.find("v=");
                if (v != std::string_view::npos)
                {
                    const std::size_t start = v + 2;
                    const std::size_t end = url.find_first_of("&#", start);
                    videoId = std::string(url.substr(start, end == std::string_view::npos ? url.size() - start : end - start));
                }
            }
            return videoId.empty() ? std::string{} : "https://i.ytimg.com/vi/" + videoId + "/hqdefault.jpg";
        }

        static MediaDiscovery currentMedia()
        {
            if (!commandExists("busctl"))
                return {.Status = "busctl not found"};
            const auto players = mprisPlayers();
            if (players.empty())
                return {.Status = "No MPRIS players on user D-Bus"};
            std::optional<MediaInfo> fallback;
            for (const auto& player : players)
            {
                MediaInfo media;
                media.Player = player;
                media.Status = scalarProperty(mprisProperty(player, "PlaybackStatus"));
                const auto metadata = mprisProperty(player, "Metadata");
                media.Title = metadataString(metadata, "xesam:title");
                media.Artist = metadataString(metadata, "xesam:artist");
                media.ArtworkUrl = metadataString(metadata, "mpris:artUrl");
                if (media.ArtworkUrl.empty())
                    media.ArtworkUrl = youtubeThumbnail(metadataString(metadata, "xesam:url"));
                if (!fallback)
                    fallback = media;
                if (media.Status == "Playing")
                    return {.Media = std::move(media), .Status = "MPRIS active"};
            }
            if (fallback)
                return {.Media = std::move(fallback), .Status = "MPRIS active"};
            return {.Status = "MPRIS players found, properties unavailable"};
        }

        static std::vector<std::byte> readArtwork(const std::string& url)
        {
            if (url.starts_with("file://"))
            {
                const std::filesystem::path path(percentDecode(std::string_view(url).substr(7)));
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file)
                    return {};
                const auto size = file.tellg();
                if (size <= 0 || size > 16 * 1024 * 1024)
                    return {};
                std::vector<std::byte> data(static_cast<std::size_t>(size));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(data.data()), size);
                return file ? data : std::vector<std::byte>{};
            }
            if (url.starts_with("http://") || url.starts_with("https://"))
                return readCommand("curl -fsSL --max-time 5 -- " + shellQuote(url) + " 2>/dev/null");
            return {};
        }

        static std::optional<Color32> dominantColor(const std::span<const std::byte> encoded)
        {
#if QUARTZ_HAS_STB_IMAGE
            if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return std::nullopt;
            int width;
            int height;
            int channels;
            stbi_uc* image = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()), static_cast<int>(encoded.size()), &width, &height, &channels, 3);
            if (!image || width <= 0 || height <= 0)
            {
                if (image)
                    stbi_image_free(image);
                return std::nullopt;
            }
            std::unordered_map<int, Bucket> buckets;
            const int stepX = std::max(1, width / 32);
            const int stepY = std::max(1, height / 32);
            for (int y = 0; y < height; y += stepY)
            {
                for (int x = 0; x < width; x += stepX)
                {
                    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3;
                    const std::uint8_t r = image[offset + 0];
                    const std::uint8_t g = image[offset + 1];
                    const std::uint8_t b = image[offset + 2];
                    const float maximum = std::max({r, g, b}) / 255.0f;
                    const float minimum = std::min({r, g, b}) / 255.0f;
                    const float saturation = maximum > 0.0f ? (maximum - minimum) / maximum : 0.0f;
                    if (maximum < 0.06f || (maximum > 0.95f && saturation < 0.08f))
                        continue;
                    const int key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                    auto& bucket = buckets[key];
                    bucket.R += r;
                    bucket.G += g;
                    bucket.B += b;
                    ++bucket.Count;
                    bucket.Score += (0.15f + saturation * 0.85f) * (0.40f + maximum * 0.60f);
                }
            }
            stbi_image_free(image);
            const Bucket* dominant = nullptr;
            for (const auto& [_, bucket] : buckets)
            {
                if (!dominant || bucket.Score > dominant->Score)
                    dominant = &bucket;
            }
            if (!dominant || dominant->Count == 0)
                return std::nullopt;
            return Color32{
                static_cast<std::uint8_t>(dominant->R / dominant->Count),
                static_cast<std::uint8_t>(dominant->G / dominant->Count),
                static_cast<std::uint8_t>(dominant->B / dominant->Count)
            };
#else
            (void)encoded;
            return std::nullopt;
#endif
        }

        static Color32 enhanceColor(const Color32 color) noexcept
        {
            auto hsv = rgbToHsv(color);
            hsv.S = std::clamp(hsv.S * 1.35f, 0.65f, 1.0f);
            return hsvToRgb(hsv.H, hsv.S, 1.0f);
        }

        void setStatus(std::string status, std::string title = {})
        {
            std::lock_guard lock(_stateMutex);
            _status = std::move(status);
            _mediaTitle = std::move(title);
        }

        void refresh()
        {
            const auto discovery = currentMedia();
            if (!discovery.Media)
            {
                _playing.store(false, std::memory_order_release);
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus(discovery.Status);
                return;
            }
            const auto& media = *discovery.Media;
            if (media.Status != "Playing")
            {
                _playing.store(false, std::memory_order_release);
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus("MPRIS " + (media.Status.empty() ? std::string("not playing") : media.Status) + " (" + media.Player + ")", media.Title);
                return;
            }
            _playing.store(true, std::memory_order_release);
            if (media.ArtworkUrl.empty())
            {
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus("MPRIS active, no artwork (" + media.Player + ")", media.Title);
                return;
            }
            const std::string mediaKey = media.Player + '\n' + media.Title + '\n' + media.Artist + '\n' + media.ArtworkUrl;
            if (mediaKey == _lastMediaKey)
                return;
#if !QUARTZ_HAS_STB_IMAGE
            setStatus("stb_image.h not available", media.Title);
            return;
#else
            const auto artwork = readArtwork(media.ArtworkUrl);
            const auto dominant = dominantColor(artwork);
            if (!dominant)
            {
                setStatus("Artwork decode failed (" + media.Player + ")", media.Title);
                return;
            }
            const auto target = enhanceColor(*dominant);
            _lastMediaKey = mediaKey;
            _targetColor.store((static_cast<std::int32_t>(target.R) << 16) | (static_cast<std::int32_t>(target.G) << 8) | target.B, std::memory_order_release);
            setStatus("Artwork color active (" + media.Player + ")", media.Title);
#endif
        }

        void run()
        {
            while (_running.load(std::memory_order_acquire))
            {
                refresh();
                const int milliseconds = _pollMilliseconds.load(std::memory_order_acquire);
                int slept = 0;
                while (_running.load(std::memory_order_acquire) && slept < milliseconds)
                {
                    constexpr int Slice = 50;
                    std::this_thread::sleep_for(std::chrono::milliseconds(Slice));
                    slept += Slice;
                }
            }
        }

        std::thread _thread;
        std::atomic_bool _running = false;
        std::atomic_int _pollMilliseconds = 500;
        std::atomic<std::int32_t> _targetColor = -1;
        std::atomic_bool _playing = false;
        mutable std::mutex _stateMutex;
        std::string _lastMediaKey;
        std::string _mediaTitle;
        std::string _status = "Starting";
    };


    enum class RuntimeSourceKind : int
    {
        Constant,
        Time,
        Audio,
        Media,
        Keyboard,
        RPC,
        Host,
        USB,
        RGB,
        NativeProcess,
        BindingStatus
    };

    enum class ProcessValueType : int
    {
        U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Bool
    };

    enum class ProcessAddressMode : int
    {
        AddressChain,
        Signature
    };

    enum class SignatureResultMode : int
    {
        MatchAddress,
        RipRelative32,
        PointerAtOffset,
        RegisterRelativeCapture
    };

    enum class RuntimeX64Register : int
    {
        Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15
    };

    enum class RuntimeDisplacementType : int
    {
        I8, I32, Manual
    };

    enum class RuntimeParameterSlot : int
    {
        Normalize,
        InputMin,
        InputMax,
        Invert,
        Scale,
        Offset,
        Clamp,
        OutputMin,
        OutputMax,
        SmoothingHz,
        UpdateHz,
        Count
    };

    enum class RuntimeControlCondition : int
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Between,
        Outside,
        RisingEdge,
        FallingEdge
    };

    enum class RuntimeControlTarget : int
    {
        ActiveShader,
        BindingEnabled,
        GlobalBrightness,
        SendFramebuffer,
        BaseColorMode,
        MaterialParameter
    };

    struct RuntimeParameterLink
    {
        bool Enabled = false;
        std::uint64_t BindingId = 0;
    };

    enum class ProcessRebindMode : int
    {
        NameExact,
        ExecutableExact,
        TitleExact,
        CommandLineExact,
        NameRegex,
        ExecutableRegex,
        TitleRegex,
        CommandLineRegex,
        AnyRegex
    };

    struct RuntimeProcessInfo
    {
        pid_t Pid = 0;
        std::string Name;
        std::string Exe;
        std::string Title;
        std::string CommandLine;
        std::string SearchText;
    };

    struct RuntimeProcessModule
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        std::string Name;
        std::string Path;
    };

    struct RuntimeProcessRegion
    {
        std::uintptr_t Base = 0;
        std::uintptr_t End = 0;
        bool Readable = false;
        bool Executable = false;
        std::string Path;
    };

    struct RuntimeRegisterCaptureState
    {
        std::mutex Mutex;
        bool Finished = false;
        bool Success = false;
        std::uint64_t RegisterValue = 0;
        std::intptr_t Displacement = 0;
        std::uintptr_t ResolvedAddress = 0;
        std::string Status;
        // Keep the worker last: members are destroyed in reverse order, so jthread requests stop
        // and joins while Mutex/Status/the result fields are still alive. The worker captures this state raw.
        std::jthread Worker;
    };

    struct RuntimeBinding
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        RuntimeSourceKind Source = RuntimeSourceKind::Constant;
        int Signal = 0;
        float Constant = 0.5f;
        char Name[64] = "Binding";
        char TargetId[128] = "source.health";
        int TargetComponent = 0;

        int ProcessId = 0;
        bool AutoReattach = true;
        ProcessRebindMode RebindMode = ProcessRebindMode::NameExact;
        ProcessValueType ValueType = ProcessValueType::Float;
        char ProcessName[128]{};
        char ProcessRebindPattern[512]{};
        char ProcessSearch[256]{};
        char Module[192]{};
        ProcessAddressMode AddressMode = ProcessAddressMode::AddressChain;
        char Address[256] = "+0x0";
        char Signature[512]{};
        bool SignatureExecutableOnly = true;
        SignatureResultMode SignatureResolve = SignatureResultMode::MatchAddress;
        int SignatureResultOffset = 0;
        int SignatureInstructionSize = 7;
        RuntimeX64Register SignatureRegister = RuntimeX64Register::R15;
        int SignatureRegisterDisplacementOffset = 3;
        RuntimeDisplacementType SignatureDisplacementType = RuntimeDisplacementType::I32;
        int SignatureManualDisplacement = 0;
        float SignatureCaptureTimeoutSeconds = 10.0f;
        float SignatureRetrySeconds = 1.0f;
        double NextProcessSearch = 0.0;

        std::vector<std::uint8_t> SignatureBytes;
        std::vector<std::uint8_t> SignatureMasks;
        std::vector<RuntimeProcessRegion> SignatureRegions;
        std::size_t SignatureRegionIndex = 0;
        std::uintptr_t SignatureCursor = 0;
        std::uintptr_t SignatureResolvedAddress = 0;
        std::uintptr_t SignatureMatchAddress = 0;
        std::uintptr_t SignatureInstructionAddress = 0;
        std::uint64_t SignatureCapturedRegister = 0;
        std::intptr_t SignatureCapturedDisplacement = 0;
        std::shared_ptr<RuntimeRegisterCaptureState> SignatureRegisterCapture;
        std::uint64_t SignatureConfigHash = 0;
        pid_t SignatureScanPid = 0;
        double NextSignatureScan = 0.0;
        double NextRegisterCapture = 0.0;
        std::uint64_t SignatureScannedBytes = 0;
        std::uint64_t SignatureTotalBytes = 0;
        float SignatureProgress = 0.0f;
        std::string SignatureStatus;

        std::uint64_t StatusBindingId = 0;
        bool WriteMaterial = true;

        bool Normalize = false;
        float InputMin = 0.0f;
        float InputMax = 1.0f;
        bool Invert = false;
        float Scale = 1.0f;
        float Offset = 0.0f;
        bool Clamp = true;
        float OutputMin = 0.0f;
        float OutputMax = 1.0f;
        float SmoothingHz = 8.0f;
        float UpdateHz = 60.0f;
        std::array<RuntimeParameterLink, static_cast<std::size_t>(RuntimeParameterSlot::Count)> ParameterLinks{};

        float RawValue = 0.0f;
        float Value = 0.0f;
        bool HasValue = false;
        bool LastReadSucceeded = false;
        bool RuntimeEnabled = true;
        double LastSuccessTime = 0.0;
        double NextUpdate = 0.0;
        double LastUpdate = 0.0;
        std::string Error;
    };

    struct RuntimeControlRule
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        char Name[64] = "Control";
        std::uint64_t SourceBindingId = 0;
        RuntimeControlCondition Condition = RuntimeControlCondition::Greater;
        float ValueA = 0.5f;
        float ValueB = 1.0f;
        float Tolerance = 0.001f;
        float Hysteresis = 0.0f;
        RuntimeControlTarget Target = RuntimeControlTarget::ActiveShader;
        int ShaderPresetIndex = 1;
        std::uint64_t TargetBindingId = 0;
        float TargetValue = 1.0f;
        bool TargetBool = true;
        int TargetComponent = 0;
        char TargetId[128]{};
        float TransitionSeconds = 0.35f;

        bool ConditionActive = false;
        bool PreviousInitialized = false;
        float PreviousValue = 0.0f;
    };

    struct RuntimeControlOutput
    {
        std::optional<int> ShaderPresetIndex;
        float ShaderTransitionSeconds = 0.0f;
        std::optional<float> GlobalBrightness;
        std::optional<bool> SendFramebuffer;
        std::optional<int> BaseColorMode;
    };

    struct RuntimeTimelineEvent
    {
        double Time = 0.0;
        std::string Category;
        std::string Text;
    };

    struct RuntimePacketRecord
    {
        static constexpr std::size_t CaptureBytes = 128;
        double Time = 0.0;
        bool Tx = false;
        std::uint16_t Type = 0;
        std::uint8_t Version = 0;
        std::uint32_t PacketId = 0;
        std::uint32_t ResponseFor = 0;
        std::uint32_t PayloadLength = 0;
        std::size_t Bytes = 0;
        std::size_t CapturedBytes = 0;
        std::array<std::byte, CaptureBytes> Data{};
    };

    struct RuntimeUSBRates
    {
        double TxKiB = 0.0;
        double RxKiB = 0.0;
        double TxTransfers = 0.0;
        double RxTransfers = 0.0;
    };

    struct RuntimeSignalContext
    {
        double Time = 0.0;
        float DeltaTime = 0.0f;
        AudioLevelSnapshot Audio{};
        const std::array<float, Columns>* MappedBands = nullptr;
        const std::array<float, Columns>* SmoothedBands = nullptr;
        std::optional<Color32> MediaColor;
        float MediaAmount = 0.0f;
        bool MediaPlaying = false;
        ReactiveKeyState Keys{};
        PerformanceSnapshot Performance{};
        bool HasPerformance = false;
        float AppCpu = 0.0f;
        bool USBConnected = false;
        USBStatsSnapshot USB{};
        RuntimeUSBRates USBRates{};
        const std::array<Color32, MatrixSize>* Framebuffer = nullptr;
        float EffectiveGain = 1.0f;
        float GainCorrection = 1.0f;
    };

    struct RuntimeInputAnalytics
    {
        std::array<std::uint64_t, MatrixSize> PressCount{};
        std::array<double, MatrixSize> DownSince{};
        std::array<double, MatrixSize> LastDuration{};
        std::array<float, MatrixSize> Previous{};
        std::uint64_t TotalPresses = 0;
        double LongestPress = 0.0;

        void update(const ReactiveKeyState& keys, const double now) noexcept
        {
            for (std::size_t i = 0; i < MatrixSize; ++i)
            {
                const bool down = keys.Down[i] > 0.5f;
                const bool wasDown = Previous[i] > 0.5f;
                if (down && !wasDown)
                {
                    ++PressCount[i];
                    ++TotalPresses;
                    DownSince[i] = now;
                }
                else if (!down && wasDown && DownSince[i] > 0.0)
                {
                    LastDuration[i] = std::max(now - DownSince[i], 0.0);
                    LongestPress = std::max(LongestPress, LastDuration[i]);
                    DownSince[i] = 0.0;
                }
                Previous[i] = keys.Down[i];
            }
        }

        double heldDuration(const std::size_t index, const double now) const noexcept
        {
            return index < MatrixSize && DownSince[index] > 0.0 ? std::max(now - DownSince[index], 0.0) : 0.0;
        }
    };

    struct RuntimeRGBAnalytics
    {
        std::array<Color32, MatrixSize> Previous{};
        std::array<float, 16> LumaHistogram{};
        std::uint64_t Frames = 0;
        std::uint64_t ChangedCellsTotal = 0;
        std::size_t ChangedCells = 0;
        float AverageChangedCells = 0.0f;
        float FrameRate = 0.0f;
        double LastFrameTime = 0.0;

        void update(const std::array<Color32, MatrixSize>& framebuffer, const double now) noexcept
        {
            ChangedCells = 0;
            LumaHistogram.fill(0.0f);
            for (std::size_t row = 0; row < ActiveProbeRows; ++row)
                for (std::size_t column = 0; column < Columns; ++column)
                {
                    const std::size_t index = row * Columns + column;
                    const auto& current = framebuffer[index];
                    const auto& previous = Previous[index];
                    if (current.R != previous.R || current.G != previous.G || current.B != previous.B) ++ChangedCells;
                    const float luma = (current.R / 255.0f) * 0.2126f + (current.G / 255.0f) * 0.7152f + (current.B / 255.0f) * 0.0722f;
                    const std::size_t bucket = std::min<std::size_t>(static_cast<std::size_t>(luma * LumaHistogram.size()), LumaHistogram.size() - 1);
                    LumaHistogram[bucket] += 1.0f;
                }
            constexpr float ActiveCells = static_cast<float>(ActiveProbeRows * Columns);
            for (auto& bucket : LumaHistogram) bucket /= ActiveCells;
            Previous = framebuffer;
            ++Frames;
            ChangedCellsTotal += ChangedCells;
            AverageChangedCells += (static_cast<float>(ChangedCells) - AverageChangedCells) * 0.05f;
            if (LastFrameTime > 0.0)
            {
                const double dt = now - LastFrameTime;
                if (dt > 0.000001)
                {
                    const float instantaneous = static_cast<float>(1.0 / dt);
                    FrameRate += (instantaneous - FrameRate) * 0.08f;
                }
            }
            LastFrameTime = now;
        }
    };

    static std::string runtimeLower(std::string_view value)
    {
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    static std::string runtimeRegexEscape(const std::string_view value)
    {
        static constexpr std::string_view Special = R"(\.^$|()[]*+?{})";
        std::string result;
        result.reserve(value.size() + 8);
        for (const char c : value)
        {
            if (Special.find(c) != std::string_view::npos) result.push_back('\\');
            result.push_back(c);
        }
        return result;
    }

    static bool runtimeRebindModeIsRegex(const ProcessRebindMode mode) noexcept
    {
        return mode >= ProcessRebindMode::NameRegex;
    }

    static const char* runtimeRebindModeName(const ProcessRebindMode mode) noexcept
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact: return "Process name (exact)";
        case ProcessRebindMode::ExecutableExact: return "Executable path (exact)";
        case ProcessRebindMode::TitleExact: return "Process title / argv[0] (exact)";
        case ProcessRebindMode::CommandLineExact: return "Command line (exact)";
        case ProcessRebindMode::NameRegex: return "Process name (regex)";
        case ProcessRebindMode::ExecutableRegex: return "Executable path (regex)";
        case ProcessRebindMode::TitleRegex: return "Process title / argv[0] (regex)";
        case ProcessRebindMode::CommandLineRegex: return "Command line (regex)";
        case ProcessRebindMode::AnyRegex: return "Any field (regex)";
        }
        return "Process name (exact)";
    }

    static std::vector<RuntimeProcessInfo> enumerateRuntimeProcesses()
    {
        std::vector<RuntimeProcessInfo> processes;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", error))
        {
            if (error || !entry.is_directory(error)) continue;
            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::ranges::all_of(name, [](const unsigned char c) { return std::isdigit(c) != 0; })) continue;
            int pid = 0;
            const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), pid);
            if (ec != std::errc{} || ptr != name.data() + name.size() || pid <= 0) continue;

            RuntimeProcessInfo process;
            process.Pid = static_cast<pid_t>(pid);
            std::ifstream comm(entry.path() / "comm");
            std::getline(comm, process.Name);
            std::array<char, 4096> exe{};
            const ssize_t count = ::readlink((entry.path() / "exe").c_str(), exe.data(), exe.size() - 1);
            if (count > 0) process.Exe.assign(exe.data(), static_cast<std::size_t>(count));

            std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
            if (cmdline)
            {
                std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
                if (!raw.empty())
                {
                    const std::size_t firstEnd = raw.find('\0');
                    process.Title = raw.substr(0, firstEnd == std::string::npos ? raw.size() : firstEnd);
                    for (char& c : raw) if (c == '\0') c = ' ';
                    process.CommandLine = trim(std::move(raw));
                }
            }
            if (process.Title.empty()) process.Title = process.Name;
            if (process.CommandLine.empty()) process.CommandLine = !process.Exe.empty() ? process.Exe : process.Name;
            if (process.Name.empty() && process.Exe.empty() && process.Title.empty()) continue;
            process.SearchText = runtimeLower(std::to_string(process.Pid) + "\n" + process.Name + "\n" + process.Exe + "\n" + process.Title + "\n" + process.CommandLine);
            processes.emplace_back(std::move(process));
        }
        std::ranges::sort(processes, [](const auto& a, const auto& b)
        {
            if (a.Name != b.Name) return a.Name < b.Name;
            return a.Pid < b.Pid;
        });
        return processes;
    }

    static std::string runtimeProcessDisplayTitle(const RuntimeProcessInfo& process)
    {
        std::string title = process.Name.empty() ? "<unnamed>" : process.Name;
        if (!process.Title.empty() && process.Title != process.Name && process.Title != process.Exe)
        {
            std::string decorative = process.Title;
            if (decorative.size() > 72) decorative.resize(69), decorative += "...";
            title += "  -  " + decorative;
        }
        return title;
    }

    static bool runtimeProcessMatchesSearch(const RuntimeProcessInfo& process, const std::string_view loweredQuery)
    {
        return loweredQuery.empty() || process.SearchText.find(loweredQuery) != std::string::npos;
    }

    static std::string runtimeProcessRebindValue(const RuntimeProcessInfo& process, const ProcessRebindMode mode)
    {
        switch (mode)
        {
        case ProcessRebindMode::NameExact:
        case ProcessRebindMode::NameRegex: return process.Name;
        case ProcessRebindMode::ExecutableExact:
        case ProcessRebindMode::ExecutableRegex: return process.Exe;
        case ProcessRebindMode::TitleExact:
        case ProcessRebindMode::TitleRegex: return process.Title;
        case ProcessRebindMode::CommandLineExact:
        case ProcessRebindMode::CommandLineRegex: return process.CommandLine;
        case ProcessRebindMode::AnyRegex: return process.Name;
        }
        return process.Name;
    }

    static void captureRuntimeRebindPattern(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        std::string value = runtimeProcessRebindValue(process, binding.RebindMode);
        if (value.empty()) value = process.Name;
        if (runtimeRebindModeIsRegex(binding.RebindMode)) value = "^" + runtimeRegexEscape(value) + "$";
        std::snprintf(binding.ProcessRebindPattern, sizeof(binding.ProcessRebindPattern), "%s", value.c_str());
    }

    static bool runtimeProcessMatchesRebind(const RuntimeProcessInfo& process, const RuntimeBinding& binding, const std::regex* regex)
    {
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            if (!regex) return false;
            const auto matches = [&](const std::string& value) { return !value.empty() && std::regex_search(value, *regex); };
            switch (binding.RebindMode)
            {
            case ProcessRebindMode::NameRegex: return matches(process.Name);
            case ProcessRebindMode::ExecutableRegex: return matches(process.Exe);
            case ProcessRebindMode::TitleRegex: return matches(process.Title);
            case ProcessRebindMode::CommandLineRegex: return matches(process.CommandLine);
            case ProcessRebindMode::AnyRegex: return matches(process.Name) || matches(process.Exe) || matches(process.Title) || matches(process.CommandLine);
            default: return false;
            }
        }
        return runtimeProcessRebindValue(process, binding.RebindMode) == binding.ProcessRebindPattern;
    }

    static std::optional<RuntimeProcessInfo> findRuntimeRebindProcess(const RuntimeBinding& binding, std::string& error)
    {
        if (!binding.AutoReattach) { error = "automatic process rebind is disabled"; return std::nullopt; }
        if (binding.ProcessRebindPattern[0] == '\0') { error = "rebind pattern is empty"; return std::nullopt; }
        std::optional<std::regex> regex;
        if (runtimeRebindModeIsRegex(binding.RebindMode))
        {
            try { regex.emplace(binding.ProcessRebindPattern, std::regex::ECMAScript | std::regex::icase); }
            catch (const std::regex_error& e) { error = std::string("invalid rebind regex: ") + e.what(); return std::nullopt; }
        }
        std::optional<RuntimeProcessInfo> best;
        for (const auto& process : enumerateRuntimeProcesses())
            if (runtimeProcessMatchesRebind(process, binding, regex ? &*regex : nullptr) && (!best || process.Pid > best->Pid)) best = process;
        if (!best) error = "waiting for process match: " + std::string(binding.ProcessRebindPattern);
        return best;
    }

    static bool runtimeProcessIsAlive(const pid_t pid) noexcept
    {
        if (pid <= 0) return false;
        errno = 0;
        return ::kill(pid, 0) == 0 || errno != ESRCH;
    }

    static double runtimeSteadySeconds() noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static bool tryRuntimeProcessRebind(RuntimeBinding& binding, pid_t& pid, std::string& error)
    {
        if (!binding.AutoReattach) { error = "process is not running"; return false; }
        const double now = runtimeSteadySeconds();
        if (now < binding.NextProcessSearch) { error = "waiting for process match: " + std::string(binding.ProcessRebindPattern); return false; }
        binding.NextProcessSearch = now + 1.0;
        const auto process = findRuntimeRebindProcess(binding, error);
        if (!process) return false;
        pid = process->Pid;
        binding.ProcessId = static_cast<int>(pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process->Name.c_str());
        binding.NextProcessSearch = 0.0;
        error.clear();
        return true;
    }

    static std::vector<RuntimeProcessModule> enumerateRuntimeModules(const pid_t pid)
    {
        std::vector<RuntimeProcessModule> modules;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return modules;
        std::unordered_map<std::string, std::size_t> indices;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offsetText, device, inode;
            if (!(stream >> range >> permissions >> offsetText >> device >> inode)) continue;
            std::string path;
            std::getline(stream, path);
            path = trim(std::move(path));
            if (path.empty() || path.front() == '[') continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t start = 0, end = 0, offset = 0;
            auto parseHex = [](const std::string_view value, std::uintptr_t& result)
            {
                const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
                return ec == std::errc{} && ptr == value.data() + value.size();
            };
            if (!parseHex(std::string_view(range).substr(0, dash), start) || !parseHex(std::string_view(range).substr(dash + 1), end) || !parseHex(offsetText, offset)) continue;
            const std::uintptr_t base = start >= offset ? start - offset : start;
            const auto existing = indices.find(path);
            if (existing == indices.end())
            {
                RuntimeProcessModule module;
                module.Base = base;
                module.End = end;
                module.Path = path;
                module.Name = std::filesystem::path(path).filename().string();
                indices[path] = modules.size();
                modules.emplace_back(std::move(module));
            }
            else
            {
                auto& module = modules[existing->second];
                module.Base = std::min(module.Base, base);
                module.End = std::max(module.End, end);
            }
        }
        std::ranges::sort(modules, std::ranges::less{}, &RuntimeProcessModule::Base);
        return modules;
    }

    static std::vector<RuntimeProcessRegion> enumerateRuntimeRegions(const pid_t pid)
    {
        std::vector<RuntimeProcessRegion> regions;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return regions;
        std::string line;
        while (std::getline(maps, line))
        {
            std::istringstream stream(line);
            std::string range, permissions, offset, device, inode;
            if (!(stream >> range >> permissions >> offset >> device >> inode)) continue;
            const std::size_t dash = range.find('-');
            if (dash == std::string::npos) continue;
            std::uintptr_t base = 0, end = 0;
            const auto [basePtr, baseEc] = std::from_chars(range.data(), range.data() + dash, base, 16);
            const auto [endPtr, endEc] = std::from_chars(range.data() + dash + 1, range.data() + range.size(), end, 16);
            if (baseEc != std::errc{} || endEc != std::errc{} || base >= end) continue;
            std::string path;
            std::getline(stream, path);
            RuntimeProcessRegion region;
            region.Base = base;
            region.End = end;
            region.Readable = !permissions.empty() && permissions[0] == 'r';
            region.Executable = permissions.size() > 2 && permissions[2] == 'x';
            region.Path = trim(std::move(path));
            regions.emplace_back(std::move(region));
        }
        return regions;
    }

    static int runtimeHexNibble(const char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static bool parseRuntimeIdaSignature(const std::string_view signature, std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& masks, std::string& error)
    {
        bytes.clear();
        masks.clear();
        std::istringstream stream{std::string(signature)};
        std::string token;
        while (stream >> token)
        {
            if (token == "?" || token == "??")
            {
                bytes.push_back(0);
                masks.push_back(0);
                continue;
            }
            if (token.size() != 2)
            {
                error = "signature token must be two hex nibbles or ?: " + token;
                return false;
            }
            const bool highWildcard = token[0] == '?';
            const bool lowWildcard = token[1] == '?';
            const int high = highWildcard ? 0 : runtimeHexNibble(token[0]);
            const int low = lowWildcard ? 0 : runtimeHexNibble(token[1]);
            if (high < 0 || low < 0)
            {
                error = "invalid signature byte: " + token;
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
            masks.push_back(static_cast<std::uint8_t>((highWildcard ? 0 : 0xF0) | (lowWildcard ? 0 : 0x0F)));
        }
        if (bytes.empty())
        {
            error = "signature is empty";
            return false;
        }
        if (std::ranges::all_of(masks, [](const std::uint8_t mask) { return mask == 0; }))
        {
            error = "signature cannot contain only wildcards";
            return false;
        }
        error.clear();
        return true;
    }

    static bool readProcessMemoryBlock(const pid_t pid, const std::uintptr_t address, std::span<std::uint8_t> buffer, std::string& error)
    {
        if (buffer.empty()) return true;
        iovec local{buffer.data(), buffer.size()};
        iovec remote{reinterpret_cast<void*>(address), buffer.size()};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(buffer.size())) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(buffer.size()) + ")";
        return false;
    }

    template<typename T>
    static bool readProcessMemoryValue(pid_t pid, std::uintptr_t address, T& value, std::string& error);

    static const char* runtimeX64RegisterName(const RuntimeX64Register reg) noexcept
    {
        static constexpr const char* Names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
        return Names[std::clamp(static_cast<int>(reg), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static std::uint64_t runtimeX64RegisterValue(const user_regs_struct& regs, const RuntimeX64Register reg) noexcept
    {
        switch (reg)
        {
        case RuntimeX64Register::Rax: return regs.rax;
        case RuntimeX64Register::Rbx: return regs.rbx;
        case RuntimeX64Register::Rcx: return regs.rcx;
        case RuntimeX64Register::Rdx: return regs.rdx;
        case RuntimeX64Register::Rsi: return regs.rsi;
        case RuntimeX64Register::Rdi: return regs.rdi;
        case RuntimeX64Register::Rbp: return regs.rbp;
        case RuntimeX64Register::Rsp: return regs.rsp;
        case RuntimeX64Register::R8: return regs.r8;
        case RuntimeX64Register::R9: return regs.r9;
        case RuntimeX64Register::R10: return regs.r10;
        case RuntimeX64Register::R11: return regs.r11;
        case RuntimeX64Register::R12: return regs.r12;
        case RuntimeX64Register::R13: return regs.r13;
        case RuntimeX64Register::R14: return regs.r14;
        case RuntimeX64Register::R15: return regs.r15;
        }
        return 0;
    }

    static std::vector<pid_t> enumerateRuntimeThreads(const pid_t pid)
    {
        std::vector<pid_t> result;
        std::error_code ec;
        const std::filesystem::path taskPath = "/proc/" + std::to_string(pid) + "/task";
        for (const auto& entry : std::filesystem::directory_iterator(taskPath, ec))
        {
            const std::string name = entry.path().filename().string();
            pid_t tid = 0;
            const auto [pointer, error] = std::from_chars(name.data(), name.data() + name.size(), tid);
            if (error == std::errc{} && pointer == name.data() + name.size() && tid > 0) result.push_back(tid);
        }
        std::ranges::sort(result);
        return result;
    }

    static bool runtimePtracePeekUser(const pid_t tid, const std::size_t offset, std::uint64_t& value) noexcept
    {
        errno = 0;
        const long result = ::ptrace(PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(offset), nullptr);
        if (result == -1 && errno != 0) return false;
        value = static_cast<std::uint64_t>(static_cast<unsigned long>(result));
        return true;
    }

    static bool runtimePtracePokeUser(const pid_t tid, const std::size_t offset, const std::uint64_t value) noexcept
    {
        errno = 0;
        return ::ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset), reinterpret_cast<void*>(static_cast<std::uintptr_t>(value))) == 0;
    }

    static bool runtimeWaitForPtraceStop(const pid_t tid, std::stop_token stop, const double timeoutSeconds, int& status) noexcept
    {
        const double deadline = runtimeSteadySeconds() + timeoutSeconds;
        while (!stop.stop_requested() && runtimeSteadySeconds() < deadline)
        {
            const pid_t result = ::waitpid(tid, &status, __WALL | WNOHANG);
            if (result == tid) return WIFSTOPPED(status);
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    static void startRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, const std::intptr_t displacement)
    {
        auto state = std::make_shared<RuntimeRegisterCaptureState>();
        RuntimeRegisterCaptureState* capture = state.get();
        const RuntimeX64Register targetRegister = binding.SignatureRegister;
        const double timeoutSeconds = std::clamp(static_cast<double>(binding.SignatureCaptureTimeoutSeconds), 0.1, 120.0);
        {
            std::lock_guard lock(capture->Mutex);
            std::ostringstream status;
            status << "Waiting for " << runtimeX64RegisterName(targetRegister) << " at 0x" << std::hex << instruction;
            capture->Status = status.str();
        }
        state->Worker = std::jthread([capture, pid, instruction, displacement, targetRegister, timeoutSeconds](std::stop_token stop)
        {
            struct TracedThread
            {
                pid_t Tid = 0;
                bool Attached = false;
                bool Stopped = false;
                bool Armed = false;
                std::uint64_t Dr0 = 0;
                std::uint64_t Dr6 = 0;
                std::uint64_t Dr7 = 0;
            };

            auto finish = [&](const bool success, const std::string& status, const std::uint64_t registerValue = 0, const std::uintptr_t resolved = 0)
            {
                std::lock_guard lock(capture->Mutex);
                capture->Finished = true;
                capture->Success = success;
                capture->RegisterValue = registerValue;
                capture->Displacement = displacement;
                capture->ResolvedAddress = resolved;
                capture->Status = status;
            };

            constexpr std::size_t Dr0Offset = offsetof(struct user, u_debugreg[0]);
            constexpr std::size_t Dr6Offset = offsetof(struct user, u_debugreg[6]);
            constexpr std::size_t Dr7Offset = offsetof(struct user, u_debugreg[7]);
            std::vector<TracedThread> threads;
            std::size_t armedCount = 0;
            std::uint64_t trapCount = 0;
            std::uint64_t lastTrapRip = 0;
            std::uint64_t lastTrapDr6 = 0;
            std::size_t seizeFailures = 0;
            std::size_t setupFailures = 0;
            int lastSeizeError = 0;
            int lastSetupError = 0;

            auto markGone = [&](TracedThread& thread)
            {
                thread.Attached = false;
                thread.Stopped = false;
                if (thread.Armed && armedCount > 0) --armedCount;
                thread.Armed = false;
            };

            auto restoreDebugRegisters = [&](TracedThread& thread)
            {
                if (!thread.Armed || !thread.Stopped) return;
                runtimePtracePokeUser(thread.Tid, Dr7Offset, thread.Dr7);
                runtimePtracePokeUser(thread.Tid, Dr0Offset, thread.Dr0);
                runtimePtracePokeUser(thread.Tid, Dr6Offset, thread.Dr6);
                thread.Armed = false;
                if (armedCount > 0) --armedCount;
            };

            auto detachStopped = [&](TracedThread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return true;
                restoreDebugRegisters(thread);
                errno = 0;
                if (::ptrace(PTRACE_DETACH, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0)
                {
                    thread.Attached = false;
                    thread.Stopped = false;
                    return true;
                }
                if (errno == ESRCH || errno == ECHILD)
                {
                    markGone(thread);
                    return true;
                }
                return false;
            };

            auto continueStopped = [&](TracedThread& thread, const int signal = 0)
            {
                if (!thread.Attached || !thread.Stopped) return false;
                errno = 0;
                if (::ptrace(PTRACE_CONT, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(signal))) == 0)
                {
                    thread.Stopped = false;
                    return true;
                }
                if (errno == ESRCH || errno == ECHILD) markGone(thread);
                else detachStopped(thread, signal);
                return false;
            };

            auto stopForCleanup = [&](TracedThread& thread, int& status, const bool interruptPending = false)
            {
                status = 0;
                if (!thread.Attached) return false;
                if (thread.Stopped) return true;

                errno = 0;
                const pid_t pending = ::waitpid(thread.Tid, &status, __WALL | WNOHANG);
                if (pending == thread.Tid)
                {
                    if (WIFEXITED(status) || WIFSIGNALED(status)) { markGone(thread); return false; }
                    if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                }
                else if (pending < 0 && errno != EINTR)
                {
                    if (errno == ESRCH || errno == ECHILD) markGone(thread);
                    return false;
                }

                if (!interruptPending)
                {
                    errno = 0;
                    if (::ptrace(PTRACE_INTERRUPT, thread.Tid, nullptr, nullptr) != 0)
                    {
                        if (errno == ESRCH || errno == ECHILD) markGone(thread);
                        return false;
                    }
                }

                for (;;)
                {
                    errno = 0;
                    const pid_t result = ::waitpid(thread.Tid, &status, __WALL);
                    if (result == thread.Tid)
                    {
                        if (WIFEXITED(status) || WIFSIGNALED(status)) { markGone(thread); return false; }
                        if (WIFSTOPPED(status)) { thread.Stopped = true; return true; }
                        continue;
                    }
                    if (result < 0 && errno == EINTR) continue;
                    if (result < 0 && (errno == ESRCH || errno == ECHILD)) markGone(thread);
                    return false;
                }
            };

            auto alreadyTracked = [&](const pid_t tid)
            {
                return std::ranges::any_of(threads, [tid](const TracedThread& thread) { return thread.Tid == tid && thread.Attached; });
            };

            auto armThread = [&](const pid_t tid)
            {
                if (tid <= 0 || alreadyTracked(tid) || stop.stop_requested()) return false;
                errno = 0;
                if (::ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0)
                {
                    ++seizeFailures;
                    lastSeizeError = errno;
                    return false;
                }

                threads.push_back({});
                TracedThread& thread = threads.back();
                thread.Tid = tid;
                thread.Attached = true;

                errno = 0;
                if (::ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0)
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    if (errno == ESRCH || errno == ECHILD) markGone(thread);
                    return false;
                }

                int status = 0;
                if (!runtimeWaitForPtraceStop(tid, stop, 0.25, status))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    int cleanupStatus = 0;
                    if (stopForCleanup(thread, cleanupStatus, true))
                    {
                        const int signal = WIFSTOPPED(cleanupStatus) ? WSTOPSIG(cleanupStatus) : 0;
                        detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
                    }
                    return false;
                }
                thread.Stopped = true;

                if (!runtimePtracePeekUser(tid, Dr0Offset, thread.Dr0) || !runtimePtracePeekUser(tid, Dr6Offset, thread.Dr6) || !runtimePtracePeekUser(tid, Dr7Offset, thread.Dr7))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    const int signal = WSTOPSIG(status);
                    detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
                    return false;
                }

                std::uint64_t dr7 = thread.Dr7;
                dr7 &= ~0x3ULL;
                dr7 &= ~(0xFULL << 16);
                dr7 |= 0x1ULL;
                if (!runtimePtracePokeUser(tid, Dr0Offset, instruction) || !runtimePtracePokeUser(tid, Dr6Offset, 0) || !runtimePtracePokeUser(tid, Dr7Offset, dr7))
                {
                    ++setupFailures;
                    lastSetupError = errno;
                    detachStopped(thread);
                    return false;
                }

                thread.Armed = true;
                ++armedCount;
                const int signal = WSTOPSIG(status);
                if (!continueStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal)) return false;
                return true;
            };

            auto refreshThreads = [&]
            {
                for (const pid_t tid : enumerateRuntimeThreads(pid)) armThread(tid);
            };

            auto updateStatus = [&]
            {
                std::lock_guard lock(capture->Mutex);
                std::ostringstream status;
                status << "Waiting for " << runtimeX64RegisterName(targetRegister) << " at 0x" << std::hex << instruction << std::dec
                       << " | armed " << armedCount << "/" << threads.size() << " | traps " << trapCount;
                if (lastTrapRip != 0) status << " | last RIP 0x" << std::hex << lastTrapRip << " DR6 0x" << lastTrapDr6 << std::dec;
                if (seizeFailures != 0) status << " | seize failures " << seizeFailures << " (" << std::strerror(lastSeizeError) << ')';
                if (setupFailures != 0) status << " | setup failures " << setupFailures << (lastSetupError ? std::string(" (") + std::strerror(lastSetupError) + ')' : std::string{});
                capture->Status = status.str();
            };

            refreshThreads();
            if (threads.empty())
            {
                finish(false, std::string("ptrace seize failed: ") + std::strerror(lastSeizeError ? lastSeizeError : errno));
                return;
            }

            bool success = false;
            std::uint64_t capturedRegister = 0;
            std::uintptr_t resolvedAddress = 0;
            std::string failure;
            if (armedCount == 0) failure = "could not arm a hardware execution breakpoint";
            const double deadline = runtimeSteadySeconds() + timeoutSeconds;
            double nextThreadRefresh = runtimeSteadySeconds() + 0.10;
            double nextStatusUpdate = 0.0;

            while (!stop.stop_requested() && runtimeSteadySeconds() < deadline && !success)
            {
                const double now = runtimeSteadySeconds();
                if (now >= nextThreadRefresh)
                {
                    refreshThreads();
                    nextThreadRefresh = now + 0.10;
                    if (armedCount > 0) failure.clear();
                }
                if (armedCount == 0)
                {
                    if (now >= nextStatusUpdate) { updateStatus(); nextStatusUpdate = now + 0.25; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                bool observed = false;
                for (auto& thread : threads)
                {
                    if (!thread.Attached || thread.Stopped) continue;
                    int status = 0;
                    errno = 0;
                    const pid_t result = ::waitpid(thread.Tid, &status, __WALL | WNOHANG);
                    if (result == 0) continue;
                    if (result < 0)
                    {
                        if (errno == ECHILD || errno == ESRCH) markGone(thread);
                        continue;
                    }
                    observed = true;
                    if (WIFEXITED(status) || WIFSIGNALED(status))
                    {
                        markGone(thread);
                        continue;
                    }
                    if (!WIFSTOPPED(status)) continue;
                    thread.Stopped = true;

                    if (!thread.Armed)
                    {
                        detachStopped(thread);
                        continue;
                    }

                    const int signal = WSTOPSIG(status);
                    if (signal == SIGTRAP)
                    {
                        ++trapCount;
                        std::uint64_t dr6 = 0;
                        user_regs_struct regs{};
                        const bool haveDr6 = runtimePtracePeekUser(thread.Tid, Dr6Offset, dr6);
                        const bool haveRegs = ::ptrace(PTRACE_GETREGS, thread.Tid, nullptr, &regs) == 0;
                        if (haveRegs) lastTrapRip = regs.rip;
                        if (haveDr6) lastTrapDr6 = dr6;
                        const bool slot0Hit = haveDr6 && (dr6 & 0x1ULL) != 0;
                        const bool targetRip = haveRegs && regs.rip == instruction;
                        if (haveRegs && (slot0Hit || targetRip))
                        {
                            capturedRegister = runtimeX64RegisterValue(regs, targetRegister);
                            resolvedAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(capturedRegister) + displacement);
                            success = true;
                            detachStopped(thread);
                            break;
                        }
                    }

                    const int deliverSignal = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal;
                    continueStopped(thread, deliverSignal);
                }

                if (now >= nextStatusUpdate) { updateStatus(); nextStatusUpdate = now + 0.25; }
                if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            for (auto& thread : threads)
            {
                if (!thread.Attached) continue;
                int status = 0;
                if (!stopForCleanup(thread, status)) continue;
                const int signal = WIFSTOPPED(status) ? WSTOPSIG(status) : 0;
                if (!detachStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal)) continueStopped(thread, signal == SIGTRAP || signal == SIGSTOP ? 0 : signal);
            }

            if (success)
            {
                std::ostringstream status;
                status << "Captured " << runtimeX64RegisterName(targetRegister) << "=0x" << std::hex << capturedRegister;
                if (displacement >= 0) status << " +0x" << static_cast<std::uint64_t>(displacement);
                else status << " -0x" << static_cast<std::uint64_t>(-displacement);
                status << " -> 0x" << resolvedAddress << std::dec << " | traps " << trapCount;
                finish(true, status.str(), capturedRegister, resolvedAddress);
            }
            else if (stop.stop_requested()) finish(false, "register capture cancelled");
            else if (armedCount == 0 && !failure.empty()) finish(false, failure);
            else
            {
                std::ostringstream status;
                status << "register capture timed out | armed " << armedCount << '/' << threads.size() << " | traps " << trapCount;
                if (lastTrapRip != 0) status << " | last RIP 0x" << std::hex << lastTrapRip << " DR6 0x" << lastTrapDr6;
                finish(false, status.str());
            }
        });
        binding.SignatureRegisterCapture = std::move(state);
    }

    static bool readRuntimeRegisterDisplacement(const RuntimeBinding& binding, const pid_t pid, const std::uintptr_t instruction, std::intptr_t& displacement, std::string& error)
    {
        if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual)
        {
            displacement = binding.SignatureManualDisplacement;
            error.clear();
            return true;
        }
        const std::uintptr_t address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction) + binding.SignatureRegisterDisplacementOffset);
        if (binding.SignatureDisplacementType == RuntimeDisplacementType::I8)
        {
            std::int8_t value = 0;
            if (!readProcessMemoryValue(pid, address, value, error)) return false;
            displacement = value;
        }
        else
        {
            std::int32_t value = 0;
            if (!readProcessMemoryValue(pid, address, value, error)) return false;
            displacement = value;
        }
        error.clear();
        return true;
    }

    static std::optional<std::uintptr_t> advanceRuntimeRegisterCapture(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        const double now = runtimeSteadySeconds();
        if (binding.SignatureResolvedAddress != 0) { error.clear(); return binding.SignatureResolvedAddress; }
        if (binding.SignatureInstructionAddress == 0) { error = "register capture has no instruction address"; return std::nullopt; }
        if (binding.SignatureRegisterCapture)
        {
            bool finished = false, success = false;
            std::uint64_t registerValue = 0;
            std::intptr_t displacement = 0;
            std::uintptr_t resolved = 0;
            std::string status;
            {
                std::lock_guard lock(binding.SignatureRegisterCapture->Mutex);
                finished = binding.SignatureRegisterCapture->Finished;
                success = binding.SignatureRegisterCapture->Success;
                registerValue = binding.SignatureRegisterCapture->RegisterValue;
                displacement = binding.SignatureRegisterCapture->Displacement;
                resolved = binding.SignatureRegisterCapture->ResolvedAddress;
                status = binding.SignatureRegisterCapture->Status;
            }
            binding.SignatureStatus = status;
            if (!finished) { error = status; return std::nullopt; }
            binding.SignatureRegisterCapture.reset();
            if (success && resolved != 0)
            {
                binding.SignatureCapturedRegister = registerValue;
                binding.SignatureCapturedDisplacement = displacement;
                binding.SignatureResolvedAddress = resolved;
                error.clear();
                return resolved;
            }
            binding.NextRegisterCapture = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            error = status;
            return std::nullopt;
        }
        if (now < binding.NextRegisterCapture)
        {
            const double remaining = std::max(binding.NextRegisterCapture - now, 0.0);
            std::ostringstream status;
            status << "register capture retry in " << std::fixed << std::setprecision(1) << remaining << " s";
            binding.SignatureStatus = status.str();
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        startRuntimeRegisterCapture(binding, pid, binding.SignatureInstructionAddress, binding.SignatureCapturedDisplacement);
        binding.SignatureStatus = "Waiting for instruction execution";
        error = binding.SignatureStatus;
        return std::nullopt;
    }

    static std::uint64_t runtimeSignatureConfigurationHash(const RuntimeBinding& binding)
    {
        std::size_t hash = std::hash<std::string_view>{}(binding.Signature);
        auto mix = [&](const std::size_t value) { hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2); };
        mix(std::hash<std::string_view>{}(binding.Module));
        mix(static_cast<std::size_t>(binding.SignatureExecutableOnly));
        mix(static_cast<std::size_t>(binding.SignatureResolve));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureResultOffset)));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureInstructionSize)));
        mix(static_cast<std::size_t>(binding.SignatureRegister));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureRegisterDisplacementOffset)));
        mix(static_cast<std::size_t>(binding.SignatureDisplacementType));
        mix(static_cast<std::size_t>(static_cast<std::uint32_t>(binding.SignatureManualDisplacement)));
        return static_cast<std::uint64_t>(hash);
    }

    static void resetRuntimeSignatureScan(RuntimeBinding& binding, const bool clearResolved = true)
    {
        binding.SignatureBytes.clear();
        binding.SignatureMasks.clear();
        binding.SignatureRegions.clear();
        binding.SignatureRegionIndex = 0;
        binding.SignatureCursor = 0;
        binding.SignatureScannedBytes = 0;
        binding.SignatureTotalBytes = 0;
        binding.SignatureProgress = 0.0f;
        binding.NextSignatureScan = 0.0;
        binding.NextRegisterCapture = 0.0;
        binding.SignatureRegisterCapture.reset();
        if (clearResolved)
        {
            binding.SignatureResolvedAddress = 0;
            binding.SignatureMatchAddress = 0;
            binding.SignatureInstructionAddress = 0;
            binding.SignatureCapturedRegister = 0;
            binding.SignatureCapturedDisplacement = 0;
        }
        binding.SignatureStatus.clear();
    }

    static bool prepareRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        if (!parseRuntimeIdaSignature(binding.Signature, binding.SignatureBytes, binding.SignatureMasks, error)) return false;
        binding.SignatureRegions.clear();
        binding.SignatureTotalBytes = 0;
        binding.SignatureScannedBytes = 0;
        for (auto& region : enumerateRuntimeRegions(pid))
        {
            if (!region.Readable || (binding.SignatureExecutableOnly && !region.Executable)) continue;
            if (binding.Module[0] != '\0')
            {
                const std::string module = binding.Module;
                const std::string name = region.Path.empty() ? std::string{} : std::filesystem::path(region.Path).filename().string();
                if (name != module && region.Path != module && region.Path.find(module) == std::string::npos) continue;
            }
            if (region.End - region.Base < binding.SignatureBytes.size()) continue;
            binding.SignatureTotalBytes += region.End - region.Base;
            binding.SignatureRegions.emplace_back(std::move(region));
        }
        if (binding.SignatureRegions.empty())
        {
            error = binding.Module[0] ? "no readable matching mappings for signature scan" : "no readable mappings for signature scan";
            return false;
        }
        binding.SignatureRegionIndex = 0;
        binding.SignatureCursor = binding.SignatureRegions.front().Base;
        binding.SignatureScannedBytes = 0;
        binding.SignatureProgress = 0.0f;
        binding.SignatureScanPid = pid;
        binding.SignatureConfigHash = runtimeSignatureConfigurationHash(binding);
        binding.SignatureStatus = "Scanning signature";
        error.clear();
        return true;
    }

    static bool runtimeSignatureMatches(const std::span<const std::uint8_t> data, const std::size_t offset, const RuntimeBinding& binding) noexcept
    {
        for (std::size_t i = 0; i < binding.SignatureBytes.size(); ++i)
            if ((data[offset + i] & binding.SignatureMasks[i]) != (binding.SignatureBytes[i] & binding.SignatureMasks[i])) return false;
        return true;
    }

    template<typename T>
    static bool readProcessMemoryValue(const pid_t pid, const std::uintptr_t address, T& value, std::string& error)
    {
        iovec local{&value, sizeof(T)};
        iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
        errno = 0;
        const ssize_t count = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (count == static_cast<ssize_t>(sizeof(T))) return true;
        error = count < 0 ? std::string(std::strerror(errno)) : "short read (" + std::to_string(count) + "/" + std::to_string(sizeof(T)) + ")";
        return false;
    }

    static bool parseRuntimeInteger(const std::string_view text, std::intptr_t& value)
    {
        std::string token = trim(std::string(text));
        if (token.empty()) return false;
        bool negative = false;
        if (token.front() == '+' || token.front() == '-')
        {
            negative = token.front() == '-';
            token.erase(token.begin());
        }
        int base = 10;
        if (token.starts_with("0x") || token.starts_with("0X"))
        {
            base = 16;
            token.erase(0, 2);
        }
        if (token.empty()) return false;
        std::uintptr_t raw = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), raw, base);
        if (ec != std::errc{} || ptr != token.data() + token.size()) return false;
        value = negative ? -static_cast<std::intptr_t>(raw) : static_cast<std::intptr_t>(raw);
        return true;
    }

    static std::optional<std::uintptr_t> resolveRuntimeAddress(const RuntimeBinding& binding, const pid_t pid, std::string& error, const std::optional<std::uintptr_t> baseOverride = std::nullopt)
    {
        std::string expression = trim(binding.Address);
        if (expression.empty()) { error = "empty address expression"; return std::nullopt; }

        std::vector<std::string> terms;
        for (std::size_t start = 0;;)
        {
            const std::size_t arrow = expression.find("->", start);
            terms.emplace_back(trim(expression.substr(start, arrow == std::string::npos ? std::string::npos : arrow - start)));
            if (arrow == std::string::npos) break;
            start = arrow + 2;
        }
        if (terms.empty()) { error = "invalid address expression"; return std::nullopt; }

        auto modules = enumerateRuntimeModules(pid);
        std::uintptr_t moduleBase = baseOverride.value_or(0);
        if (!baseOverride && binding.Module[0] != '\0')
        {
            const std::string wanted = binding.Module;
            const auto it = std::ranges::find_if(modules, [&](const RuntimeProcessModule& module)
            {
                return module.Name == wanted || module.Path == wanted || module.Path.find(wanted) != std::string::npos;
            });
            if (it == modules.end()) { error = "module not found: " + wanted; return std::nullopt; }
            moduleBase = it->Base;
        }

        std::string first = terms.front();
        if (const std::size_t plus = first.find('+'); plus != std::string::npos && plus > 0 && !std::isdigit(static_cast<unsigned char>(first[0])))
        {
            const std::string moduleName = trim(first.substr(0, plus));
            const auto it = std::ranges::find_if(modules, [&](const RuntimeProcessModule& module)
            {
                return module.Name == moduleName || module.Path.find(moduleName) != std::string::npos;
            });
            if (it == modules.end()) { error = "module not found: " + moduleName; return std::nullopt; }
            moduleBase = it->Base;
            first = first.substr(plus);
        }

        std::intptr_t firstValue = 0;
        if (!parseRuntimeInteger(first, firstValue)) { error = "invalid address term: " + first; return std::nullopt; }
        std::uintptr_t address = first.front() == '+' || first.front() == '-' || moduleBase != 0 ? static_cast<std::uintptr_t>(static_cast<std::intptr_t>(moduleBase) + firstValue) : static_cast<std::uintptr_t>(firstValue);

        for (std::size_t i = 1; i < terms.size(); ++i)
        {
            std::uintptr_t pointer = 0;
            if (!readProcessMemoryValue(pid, address, pointer, error)) { error = "pointer read failed at 0x" + [&]{ std::ostringstream s; s << std::hex << address; return s.str(); }() + ": " + error; return std::nullopt; }
            std::intptr_t offset = 0;
            if (!parseRuntimeInteger(terms[i], offset)) { error = "invalid pointer offset: " + terms[i]; return std::nullopt; }
            address = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(pointer) + offset);
        }
        return address;
    }

    static std::optional<std::uintptr_t> advanceRuntimeSignatureScan(RuntimeBinding& binding, const pid_t pid, std::string& error)
    {
        const double now = runtimeSteadySeconds();
        const std::uint64_t configurationHash = runtimeSignatureConfigurationHash(binding);
        if (binding.SignatureScanPid != pid || binding.SignatureConfigHash != configurationHash)
        {
            resetRuntimeSignatureScan(binding);
            binding.SignatureScanPid = pid;
            binding.SignatureConfigHash = configurationHash;
        }
        if (binding.SignatureResolvedAddress != 0)
        {
            error.clear();
            return binding.SignatureResolvedAddress;
        }
        if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0)
            return advanceRuntimeRegisterCapture(binding, pid, error);
        if (now < binding.NextSignatureScan)
        {
            const double remaining = std::max(binding.NextSignatureScan - now, 0.0);
            std::ostringstream status;
            status << "signature not found; retry in " << std::fixed << std::setprecision(1) << remaining << " s";
            binding.SignatureStatus = status.str();
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        if (binding.SignatureRegions.empty() && !prepareRuntimeSignatureScan(binding, pid, error))
        {
            binding.SignatureStatus = error;
            binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            return std::nullopt;
        }

        constexpr std::size_t ScanBudget = 256 * 1024;
        constexpr std::size_t ReadChunk = 128 * 1024;
        std::size_t budget = ScanBudget;
        std::vector<std::uint8_t> buffer;
        while (budget > 0 && binding.SignatureRegionIndex < binding.SignatureRegions.size())
        {
            const auto& region = binding.SignatureRegions[binding.SignatureRegionIndex];
            if (binding.SignatureCursor < region.Base) binding.SignatureCursor = region.Base;
            if (binding.SignatureCursor >= region.End)
            {
                ++binding.SignatureRegionIndex;
                if (binding.SignatureRegionIndex < binding.SignatureRegions.size()) binding.SignatureCursor = binding.SignatureRegions[binding.SignatureRegionIndex].Base;
                continue;
            }
            const std::size_t remaining = static_cast<std::size_t>(region.End - binding.SignatureCursor);
            const std::size_t readSize = std::min({remaining, ReadChunk, budget + binding.SignatureBytes.size() - 1});
            if (readSize < binding.SignatureBytes.size())
            {
                binding.SignatureScannedBytes += remaining;
                binding.SignatureCursor = region.End;
                continue;
            }
            buffer.resize(readSize);
            std::string readError;
            if (!readProcessMemoryBlock(pid, binding.SignatureCursor, buffer, readError))
            {
                binding.SignatureScannedBytes += remaining;
                binding.SignatureCursor = region.End;
                continue;
            }
            const std::size_t last = buffer.size() - binding.SignatureBytes.size();
            for (std::size_t offset = 0; offset <= last; ++offset)
            {
                if (!runtimeSignatureMatches(buffer, offset, binding)) continue;
                const std::uintptr_t match = binding.SignatureCursor + offset;
                std::uintptr_t resolved = 0;
                if (binding.SignatureResolve == SignatureResultMode::MatchAddress)
                    resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                else if (binding.SignatureResolve == SignatureResultMode::PointerAtOffset)
                {
                    const std::uintptr_t pointerAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    if (!readProcessMemoryValue(pid, pointerAddress, resolved, error))
                    {
                        binding.SignatureStatus = "signature matched, pointer resolve failed: " + error;
                        return std::nullopt;
                    }
                }
                else if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
                {
                    std::int32_t displacement = 0;
                    const std::uintptr_t displacementAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    if (!readProcessMemoryValue(pid, displacementAddress, displacement, error))
                    {
                        binding.SignatureStatus = "signature matched, RIP displacement read failed: " + error;
                        return std::nullopt;
                    }
                    const int instructionSize = std::max(binding.SignatureInstructionSize, 1);
                    resolved = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + instructionSize + displacement);
                }
                else
                {
                    binding.SignatureMatchAddress = match;
                    binding.SignatureInstructionAddress = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(match) + binding.SignatureResultOffset);
                    std::intptr_t displacement = 0;
                    if (!readRuntimeRegisterDisplacement(binding, pid, binding.SignatureInstructionAddress, displacement, error))
                    {
                        binding.SignatureStatus = "signature matched, displacement read failed: " + error;
                        return std::nullopt;
                    }
                    binding.SignatureCapturedDisplacement = displacement;
                    binding.SignatureProgress = 1.0f;
                    std::ostringstream status;
                    status << "Writer located 0x" << std::hex << binding.SignatureInstructionAddress << "; waiting for " << runtimeX64RegisterName(binding.SignatureRegister);
                    binding.SignatureStatus = status.str();
                    return advanceRuntimeRegisterCapture(binding, pid, error);
                }
                binding.SignatureResolvedAddress = resolved;
                binding.SignatureMatchAddress = match;
                binding.SignatureProgress = 1.0f;
                std::ostringstream status;
                status << "Signature resolved 0x" << std::hex << resolved << " from match 0x" << match;
                binding.SignatureStatus = status.str();
                error.clear();
                return resolved;
            }
            const std::size_t overlap = binding.SignatureBytes.size() > 1 ? binding.SignatureBytes.size() - 1 : 0;
            const std::size_t step = readSize > overlap ? readSize - overlap : readSize;
            binding.SignatureCursor += step;
            binding.SignatureScannedBytes += step;
            budget = step >= budget ? 0 : budget - step;
            binding.SignatureProgress = binding.SignatureTotalBytes ? std::clamp(static_cast<float>(static_cast<double>(binding.SignatureScannedBytes) / binding.SignatureTotalBytes), 0.0f, 1.0f) : 0.0f;
        }

        if (binding.SignatureRegionIndex >= binding.SignatureRegions.size())
        {
            binding.SignatureRegions.clear();
            binding.SignatureRegionIndex = 0;
            binding.SignatureCursor = 0;
            binding.NextSignatureScan = now + std::max(static_cast<double>(binding.SignatureRetrySeconds), 0.1);
            binding.SignatureProgress = 0.0f;
            binding.SignatureStatus = "Signature not found; waiting for retry interval";
            error = binding.SignatureStatus;
            return std::nullopt;
        }
        std::ostringstream status;
        status << "Scanning signature " << std::fixed << std::setprecision(0) << binding.SignatureProgress * 100.0f << "%";
        binding.SignatureStatus = status.str();
        error = binding.SignatureStatus;
        return std::nullopt;
    }

    static bool readNativeBinding(RuntimeBinding& binding, float& output)
    {
        pid_t pid = static_cast<pid_t>(binding.ProcessId);
        if (!runtimeProcessIsAlive(pid) && !tryRuntimeProcessRebind(binding, pid, binding.Error)) return false;

        std::string error;
        std::optional<std::uintptr_t> signatureBase;
        if (binding.AddressMode == ProcessAddressMode::Signature)
        {
            signatureBase = advanceRuntimeSignatureScan(binding, pid, error);
            if (!signatureBase)
            {
                binding.Error = std::move(error);
                return false;
            }
        }
        const auto address = resolveRuntimeAddress(binding, pid, error, signatureBase);
        if (!address)
        {
            if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
            {
                binding.SignatureResolvedAddress = 0;
                binding.NextRegisterCapture = 0.0;
                binding.SignatureRegisterCapture.reset();
            }
            binding.Error = std::move(error);
            return false;
        }

#define QUARTZ_READ_NATIVE(type) do { type value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = static_cast<float>(value); } while (false)
        switch (binding.ValueType)
        {
        case ProcessValueType::U8: QUARTZ_READ_NATIVE(std::uint8_t); break;
        case ProcessValueType::I8: QUARTZ_READ_NATIVE(std::int8_t); break;
        case ProcessValueType::U16: QUARTZ_READ_NATIVE(std::uint16_t); break;
        case ProcessValueType::I16: QUARTZ_READ_NATIVE(std::int16_t); break;
        case ProcessValueType::U32: QUARTZ_READ_NATIVE(std::uint32_t); break;
        case ProcessValueType::I32: QUARTZ_READ_NATIVE(std::int32_t); break;
        case ProcessValueType::U64: QUARTZ_READ_NATIVE(std::uint64_t); break;
        case ProcessValueType::I64: QUARTZ_READ_NATIVE(std::int64_t); break;
        case ProcessValueType::Float: QUARTZ_READ_NATIVE(float); break;
        case ProcessValueType::Double: { double value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = static_cast<float>(value); break; }
        case ProcessValueType::Bool: { std::uint8_t value{}; if (!readProcessMemoryValue(pid, *address, value, error)) { if (binding.AddressMode == ProcessAddressMode::Signature && binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture) { binding.SignatureResolvedAddress = 0; binding.NextRegisterCapture = 0.0; binding.SignatureRegisterCapture.reset(); } binding.Error = std::move(error); return false; } output = value != 0 ? 1.0f : 0.0f; break; }
        }
#undef QUARTZ_READ_NATIVE
        binding.Error.clear();
        return true;
    }

    static std::filesystem::path runtimeBindingsPath()
    {
        return settingsPath().parent_path() / "visualizer.bindings.ini";
    }

    static std::string runtimeEscape(const std::string_view value)
    {
        static constexpr char Hex[] = "0123456789ABCDEF";
        std::string result;
        for (const unsigned char c : value)
        {
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ' ') result.push_back(static_cast<char>(c));
            else { result.push_back('%'); result.push_back(Hex[c >> 4]); result.push_back(Hex[c & 0xF]); }
        }
        return result;
    }

    static std::string runtimeUnescape(const std::string_view value)
    {
        auto hex = [](const char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string result;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '%' && i + 2 < value.size())
            {
                const int hi = hex(value[i + 1]), lo = hex(value[i + 2]);
                if (hi >= 0 && lo >= 0) { result.push_back(static_cast<char>((hi << 4) | lo)); i += 2; continue; }
            }
            result.push_back(value[i]);
        }
        return result;
    }

    static const char* runtimeSourceName(const RuntimeSourceKind source)
    {
        switch (source)
        {
        case RuntimeSourceKind::Constant: return "Constant";
        case RuntimeSourceKind::Time: return "Time";
        case RuntimeSourceKind::Audio: return "Audio";
        case RuntimeSourceKind::Media: return "Media / MPRIS";
        case RuntimeSourceKind::Keyboard: return "Keyboard";
        case RuntimeSourceKind::RPC: return "RPC / firmware";
        case RuntimeSourceKind::Host: return "Host";
        case RuntimeSourceKind::USB: return "USB";
        case RuntimeSourceKind::RGB: return "RGB output";
        case RuntimeSourceKind::NativeProcess: return "Native process memory";
        case RuntimeSourceKind::BindingStatus: return "Binding status";
        }
        return "Unknown";
    }

    static std::vector<std::string_view> runtimeSignalNames(const RuntimeSourceKind source)
    {
        switch (source)
        {
        case RuntimeSourceKind::Constant: return {"Value"};
        case RuntimeSourceKind::Time: return {"Seconds", "Sine", "Cosine", "Saw 0..1"};
        case RuntimeSourceKind::Audio: return {"RMS", "Peak", "Bass", "Mid", "Treble", "Effective gain", "Gain correction"};
        case RuntimeSourceKind::Media: return {"Artwork amount", "Artwork R", "Artwork G", "Artwork B", "Playing"};
        case RuntimeSourceKind::Keyboard: return {"Caps Lock", "Scroll Lock", "Held fraction", "Recent key pulse"};
        case RuntimeSourceKind::RPC: return {"Keyboard CPU %", "Scan rate Hz", "Matrix us", "RGB us", "Scan period us"};
        case RuntimeSourceKind::Host: return {"App CPU %"};
        case RuntimeSourceKind::USB: return {"Connected", "TX KiB/s", "RX KiB/s", "TX transfers/s", "RX transfers/s", "Errors"};
        case RuntimeSourceKind::RGB: return {"Average luma", "Lit fraction", "Average R", "Average G", "Average B"};
        case RuntimeSourceKind::NativeProcess: return {"Value"};
        case RuntimeSourceKind::BindingStatus: return {"Has value", "Last read succeeded", "Enabled", "Process alive", "Address resolved", "Register captured", "Has error"};
        }
        return {"Value"};
    }

    class RuntimeBindingEngine
    {
    public:
        RuntimeBindingEngine() { load(); }
        ~RuntimeBindingEngine() { save(); }

        std::vector<RuntimeBinding>& bindings() noexcept { return _bindings; }
        const std::vector<RuntimeBinding>& bindings() const noexcept { return _bindings; }
        std::vector<RuntimeControlRule>& controls() noexcept { return _controls; }
        const std::vector<RuntimeControlRule>& controls() const noexcept { return _controls; }
        const RuntimeUSBRates& usbRates() const noexcept { return _usbRates; }
        std::uint64_t revision() const noexcept { return _revision; }
        const std::filesystem::path& path() const noexcept { return _path; }

        RuntimeBinding& add()
        {
            _bindings.emplace_back();
            _bindings.back().Id = _nextBindingId++;
            std::snprintf(_bindings.back().Name, sizeof(_bindings.back().Name), "Binding %zu", _bindings.size());
            ++_revision;
            return _bindings.back();
        }

        RuntimeControlRule& addControl()
        {
            _controls.emplace_back();
            auto& control = _controls.back();
            control.Id = _nextControlId++;
            std::snprintf(control.Name, sizeof(control.Name), "Control %zu", _controls.size());
            ++_revision;
            return control;
        }

        void eraseControl(const std::size_t index)
        {
            if (index >= _controls.size()) return;
            _controls.erase(_controls.begin() + static_cast<std::ptrdiff_t>(index));
            ++_revision;
        }

        void erase(const std::size_t index)
        {
            if (index >= _bindings.size()) return;
            const std::uint64_t erasedId = _bindings[index].Id;
            _bindings.erase(_bindings.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& binding : _bindings)
            {
                if (binding.StatusBindingId == erasedId) binding.StatusBindingId = 0;
                for (auto& link : binding.ParameterLinks)
                    if (link.Enabled && link.BindingId == erasedId) link = {};
            }
            for (auto& control : _controls)
            {
                if (control.SourceBindingId == erasedId) control.SourceBindingId = 0;
                if (control.TargetBindingId == erasedId) control.TargetBindingId = 0;
            }
            ++_revision;
        }

        void markChanged() noexcept { ++_revision; }

        RuntimeBinding* findBinding(const std::uint64_t id) noexcept
        {
            const auto it = std::ranges::find(_bindings, id, &RuntimeBinding::Id);
            return it == _bindings.end() ? nullptr : &*it;
        }

        const RuntimeBinding* findBinding(const std::uint64_t id) const noexcept
        {
            const auto it = std::ranges::find(_bindings, id, &RuntimeBinding::Id);
            return it == _bindings.end() ? nullptr : &*it;
        }

        bool canParameterLink(const std::uint64_t ownerId, const std::uint64_t sourceId) const
        {
            if (ownerId == 0 || sourceId == 0 || ownerId == sourceId || !findBinding(sourceId)) return false;
            std::set<std::uint64_t> visited;
            return !bindingDependsOn(sourceId, ownerId, visited);
        }

        bool setParameterLink(RuntimeBinding& owner, const RuntimeParameterSlot slot, const std::uint64_t sourceId)
        {
            auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
            if (sourceId == 0)
            {
                link = {};
                ++_revision;
                return true;
            }
            if (!canParameterLink(owner.Id, sourceId)) return false;
            link.Enabled = true;
            link.BindingId = sourceId;
            ++_revision;
            return true;
        }

        float parameterValue(const RuntimeBinding& owner, const RuntimeParameterSlot slot, const float fallback) const noexcept
        {
            const auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
            if (!link.Enabled || link.BindingId == 0) return fallback;
            const RuntimeBinding* source = findBinding(link.BindingId);
            return source && source->Enabled && source->HasValue ? source->Value : fallback;
        }

        bool parameterBool(const RuntimeBinding& owner, const RuntimeParameterSlot slot, const bool fallback) const noexcept
        {
            return parameterValue(owner, slot, fallback ? 1.0f : 0.0f) >= 0.5f;
        }

        void applyMaterialValues(ShaderFramebuffer& shader) const noexcept
        {
            for (const auto& binding : _bindings)
                if (binding.Enabled && binding.RuntimeEnabled && binding.WriteMaterial && binding.HasValue)
                    shader.setMaterialParameter(binding.TargetId, binding.TargetComponent, binding.Value);
        }

        void updateRates(const USBStatsSnapshot& stats, const double now)
        {
            if (_rateTime <= 0.0)
            {
                _lastUSB = stats;
                _rateTime = now;
                return;
            }
            const double elapsed = now - _rateTime;
            if (elapsed < 0.25) return;
            _usbRates.TxKiB = (stats.TxBytes - _lastUSB.TxBytes) / 1024.0 / elapsed;
            _usbRates.RxKiB = (stats.RxBytes - _lastUSB.RxBytes) / 1024.0 / elapsed;
            _usbRates.TxTransfers = (stats.TxTransfers - _lastUSB.TxTransfers) / elapsed;
            _usbRates.RxTransfers = (stats.RxTransfers - _lastUSB.RxTransfers) / elapsed;
            _lastUSB = stats;
            _rateTime = now;
        }

        void update(const RuntimeSignalContext& context, ShaderFramebuffer& shader)
        {
            for (auto& binding : _bindings)
            {
                if (!binding.Enabled || !binding.RuntimeEnabled || context.Time < binding.NextUpdate) continue;
                const float updateHz = std::clamp(parameterValue(binding, RuntimeParameterSlot::UpdateHz, binding.UpdateHz), 0.5f, 500.0f);
                binding.NextUpdate = context.Time + 1.0 / updateHz;

                float raw = 0.0f;
                binding.LastReadSucceeded = false;
                if (!readSource(binding, context, raw)) continue;
                binding.LastReadSucceeded = true;
                binding.LastSuccessTime = context.Time;
                binding.RawValue = raw;

                const bool normalize = parameterBool(binding, RuntimeParameterSlot::Normalize, binding.Normalize);
                const float inputMin = parameterValue(binding, RuntimeParameterSlot::InputMin, binding.InputMin);
                const float inputMax = parameterValue(binding, RuntimeParameterSlot::InputMax, binding.InputMax);
                const bool invert = parameterBool(binding, RuntimeParameterSlot::Invert, binding.Invert);
                const float scale = parameterValue(binding, RuntimeParameterSlot::Scale, binding.Scale);
                const float offset = parameterValue(binding, RuntimeParameterSlot::Offset, binding.Offset);
                const bool clampOutput = parameterBool(binding, RuntimeParameterSlot::Clamp, binding.Clamp);
                const float outputMin = parameterValue(binding, RuntimeParameterSlot::OutputMin, binding.OutputMin);
                const float outputMax = parameterValue(binding, RuntimeParameterSlot::OutputMax, binding.OutputMax);
                const float smoothingHz = std::max(parameterValue(binding, RuntimeParameterSlot::SmoothingHz, binding.SmoothingHz), 0.0f);

                float transformed = raw;
                if (normalize)
                {
                    const float range = inputMax - inputMin;
                    transformed = std::abs(range) > 0.000001f ? (transformed - inputMin) / range : 0.0f;
                }
                if (invert) transformed = 1.0f - transformed;
                transformed = transformed * scale + offset;
                if (clampOutput) transformed = std::clamp(transformed, std::min(outputMin, outputMax), std::max(outputMin, outputMax));

                const float dt = binding.LastUpdate > 0.0 ? static_cast<float>(std::clamp(context.Time - binding.LastUpdate, 0.0001, 1.0)) : 1.0f / updateHz;
                binding.LastUpdate = context.Time;
                if (!binding.HasValue)
                {
                    binding.Value = transformed;
                    binding.HasValue = true;
                }
                else
                {
                    const float alpha = smoothingHz <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothingHz * dt);
                    binding.Value += (transformed - binding.Value) * alpha;
                }
                if (binding.WriteMaterial)
                {
                    if (!shader.setMaterialParameter(binding.TargetId, binding.TargetComponent, binding.Value)) binding.Error = "material id/component not active in current shader";
                    else binding.Error.clear();
                }
                else
                    binding.Error.clear();
            }
        }

        RuntimeControlOutput evaluateControls(ShaderFramebuffer& shader)
        {
            for (auto& binding : _bindings) binding.RuntimeEnabled = true;
            RuntimeControlOutput output;
            for (auto& control : _controls)
            {
                if (!control.Enabled) continue;
                RuntimeBinding* source = findBinding(control.SourceBindingId);
                if (!source || !source->Enabled || !source->HasValue) continue;
                const float value = source->Value;
                if (!evaluateControlCondition(control, value)) continue;

                switch (control.Target)
                {
                case RuntimeControlTarget::ActiveShader:
                    if (control.ShaderPresetIndex > 0 && control.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size()))
                    {
                        output.ShaderPresetIndex = control.ShaderPresetIndex;
                        output.ShaderTransitionSeconds = std::clamp(control.TransitionSeconds, 0.0f, 10.0f);
                    }
                    break;
                case RuntimeControlTarget::BindingEnabled:
                    if (RuntimeBinding* target = findBinding(control.TargetBindingId); target && target != source) target->RuntimeEnabled = control.TargetBool;
                    break;
                case RuntimeControlTarget::GlobalBrightness:
                    output.GlobalBrightness = std::clamp(control.TargetValue, 0.0f, 1.0f);
                    break;
                case RuntimeControlTarget::SendFramebuffer:
                    output.SendFramebuffer = control.TargetBool;
                    break;
                case RuntimeControlTarget::BaseColorMode:
                    output.BaseColorMode = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2);
                    break;
                case RuntimeControlTarget::MaterialParameter:
                    shader.setMaterialParameter(control.TargetId, control.TargetComponent, control.TargetValue);
                    break;
                }
            }
            return output;
        }

        bool save()
        {
            std::error_code ec;
            std::filesystem::create_directories(_path.parent_path(), ec);
            const auto temporary = std::filesystem::path(_path.string() + ".tmp");
            std::ofstream file(temporary, std::ios::trunc);
            if (!file) return false;
            file << "# Quartz runtime material bindings v5\n";
            for (const auto& b : _bindings)
            {
                file << "B\t" << b.Enabled << '\t' << static_cast<int>(b.Source) << '\t' << b.Signal << '\t' << b.Constant << '\t'
                     << runtimeEscape(b.Name) << '\t' << runtimeEscape(b.TargetId) << '\t' << b.TargetComponent << '\t'
                     << b.ProcessId << '\t' << b.AutoReattach << '\t' << static_cast<int>(b.ValueType) << '\t'
                     << runtimeEscape(b.ProcessName) << '\t' << runtimeEscape(b.Module) << '\t' << runtimeEscape(b.Address) << '\t'
                     << "" << '\t' << "" << '\t' << 0 << '\t' << false << '\t'
                     << b.Normalize << '\t' << b.InputMin << '\t' << b.InputMax << '\t' << b.Invert << '\t' << b.Scale << '\t' << b.Offset << '\t'
                     << b.Clamp << '\t' << b.OutputMin << '\t' << b.OutputMax << '\t' << b.SmoothingHz << '\t' << b.UpdateHz << '\t'
                     << static_cast<int>(b.RebindMode) << '\t' << runtimeEscape(b.ProcessRebindPattern) << '\t' << b.Id << '\t'
                     << static_cast<int>(b.AddressMode) << '\t' << runtimeEscape(b.Signature) << '\t' << b.SignatureExecutableOnly << '\t'
                     << static_cast<int>(b.SignatureResolve) << '\t' << b.SignatureResultOffset << '\t' << b.SignatureInstructionSize << '\t' << b.SignatureRetrySeconds << '\t'
                     << runtimeEscape(serializeParameterLinks(b)) << '\t' << static_cast<int>(b.SignatureRegister) << '\t' << b.SignatureRegisterDisplacementOffset << '\t'
                     << static_cast<int>(b.SignatureDisplacementType) << '\t' << b.SignatureManualDisplacement << '\t' << b.SignatureCaptureTimeoutSeconds << '\t' << b.StatusBindingId << '\t' << b.WriteMaterial << '\n';
            }
            for (const auto& c : _controls)
            {
                file << "C\t" << c.Enabled << '\t' << c.Id << '\t' << runtimeEscape(c.Name) << '\t' << c.SourceBindingId << '\t'
                     << static_cast<int>(c.Condition) << '\t' << c.ValueA << '\t' << c.ValueB << '\t' << c.Tolerance << '\t' << c.Hysteresis << '\t'
                     << static_cast<int>(c.Target) << '\t' << c.ShaderPresetIndex << '\t' << c.TargetBindingId << '\t' << c.TargetValue << '\t' << c.TargetBool << '\t'
                     << c.TargetComponent << '\t' << runtimeEscape(c.TargetId) << '\t' << c.TransitionSeconds << '\n';
            }
            file.close();
            if (!file) return false;
            std::filesystem::rename(temporary, _path, ec);
            if (ec)
            {
                std::filesystem::remove(_path, ec);
                ec.clear();
                std::filesystem::rename(temporary, _path, ec);
            }
            if (!ec) _savedRevision = _revision;
            return !ec;
        }

        void saveIfChanged() { if (_savedRevision != _revision) save(); }

    private:
        template<std::size_t N>
        static void copyField(char (&destination)[N], const std::string& value)
        {
            std::snprintf(destination, N, "%s", value.c_str());
        }

        void load()
        {
            _path = runtimeBindingsPath();
            std::ifstream file(_path);
            if (!file) return;
            std::string line;
            while (std::getline(file, line))
            {
                if (line.starts_with("B\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 27) continue;
                    RuntimeBinding b;
                    auto parseInt = [&](const std::size_t index, int& value) { return index < fields.size() && parseNumber(fields[index], value); };
                    auto parseFloat = [&](const std::size_t index, float& value) { return index < fields.size() && parseNumber(fields[index], value); };
                    auto parseB = [&](const std::size_t index, bool& value) { return index < fields.size() && parseBool(fields[index], value); };
                    int source = 0, valueType = 0;
                    if (!parseB(0, b.Enabled) || !parseInt(1, source) || !parseInt(2, b.Signal) || !parseFloat(3, b.Constant)) continue;
                    b.Source = static_cast<RuntimeSourceKind>(std::clamp(source, 0, static_cast<int>(RuntimeSourceKind::BindingStatus)));
                    copyField(b.Name, runtimeUnescape(fields[4])); copyField(b.TargetId, runtimeUnescape(fields[5]));
                    parseInt(6, b.TargetComponent); parseInt(7, b.ProcessId); parseB(8, b.AutoReattach); parseInt(9, valueType);
                    b.ValueType = static_cast<ProcessValueType>(std::clamp(valueType, 0, static_cast<int>(ProcessValueType::Bool)));
                    copyField(b.ProcessName, runtimeUnescape(fields[10])); copyField(b.Module, runtimeUnescape(fields[11])); copyField(b.Address, runtimeUnescape(fields[12]));
                    // fields 13..16 were the removed CLR type/member/instance/static fields. Keep their slots for v4 compatibility.
                    parseB(17, b.Normalize); parseFloat(18, b.InputMin); parseFloat(19, b.InputMax);
                    parseB(20, b.Invert); parseFloat(21, b.Scale); parseFloat(22, b.Offset); parseB(23, b.Clamp); parseFloat(24, b.OutputMin); parseFloat(25, b.OutputMax);
                    if (fields.size() > 26) parseFloat(26, b.SmoothingHz);
                    if (fields.size() > 27) parseFloat(27, b.UpdateHz);
                    int rebindMode = static_cast<int>(ProcessRebindMode::NameExact);
                    if (fields.size() > 28) parseInt(28, rebindMode);
                    b.RebindMode = static_cast<ProcessRebindMode>(std::clamp(rebindMode, 0, static_cast<int>(ProcessRebindMode::AnyRegex)));
                    if (fields.size() > 29) copyField(b.ProcessRebindPattern, runtimeUnescape(fields[29]));
                    if (b.ProcessRebindPattern[0] == '\0' && b.ProcessName[0] != '\0') std::snprintf(b.ProcessRebindPattern, sizeof(b.ProcessRebindPattern), "%s", b.ProcessName);
                    if (fields.size() > 30) parseNumber(fields[30], b.Id);
                    int addressMode = static_cast<int>(ProcessAddressMode::AddressChain);
                    if (fields.size() > 31) parseInt(31, addressMode);
                    b.AddressMode = static_cast<ProcessAddressMode>(std::clamp(addressMode, 0, static_cast<int>(ProcessAddressMode::Signature)));
                    if (fields.size() > 32) copyField(b.Signature, runtimeUnescape(fields[32]));
                    if (fields.size() > 33) parseB(33, b.SignatureExecutableOnly);
                    int signatureResolve = static_cast<int>(SignatureResultMode::MatchAddress);
                    if (fields.size() > 34) parseInt(34, signatureResolve);
                    b.SignatureResolve = static_cast<SignatureResultMode>(std::clamp(signatureResolve, 0, static_cast<int>(SignatureResultMode::RegisterRelativeCapture)));
                    if (fields.size() > 35) parseInt(35, b.SignatureResultOffset);
                    if (fields.size() > 36) parseInt(36, b.SignatureInstructionSize);
                    if (fields.size() > 37) parseFloat(37, b.SignatureRetrySeconds);
                    if (fields.size() > 38) parseParameterLinks(runtimeUnescape(fields[38]), b);
                    int signatureRegister = static_cast<int>(RuntimeX64Register::R15);
                    if (fields.size() > 39) parseInt(39, signatureRegister);
                    b.SignatureRegister = static_cast<RuntimeX64Register>(std::clamp(signatureRegister, 0, static_cast<int>(RuntimeX64Register::R15)));
                    if (fields.size() > 40) parseInt(40, b.SignatureRegisterDisplacementOffset);
                    int displacementType = static_cast<int>(RuntimeDisplacementType::I32);
                    if (fields.size() > 41) parseInt(41, displacementType);
                    b.SignatureDisplacementType = static_cast<RuntimeDisplacementType>(std::clamp(displacementType, 0, static_cast<int>(RuntimeDisplacementType::Manual)));
                    if (fields.size() > 42) parseInt(42, b.SignatureManualDisplacement);
                    if (fields.size() > 43) parseFloat(43, b.SignatureCaptureTimeoutSeconds);
                    if (fields.size() > 44) parseNumber(fields[44], b.StatusBindingId);
                    if (fields.size() > 45) parseB(45, b.WriteMaterial);
                    if (b.Id == 0) b.Id = _nextBindingId++;
                    else _nextBindingId = std::max(_nextBindingId, b.Id + 1);
                    _bindings.emplace_back(std::move(b));
                }
                else if (line.starts_with("C\t"))
                {
                    std::vector<std::string> fields;
                    std::size_t start = 2;
                    for (;;)
                    {
                        const std::size_t tab = line.find('\t', start);
                        fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (fields.size() < 15) continue;
                    RuntimeControlRule c;
                    int condition = 0, target = 0;
                    if (!parseBool(fields[0], c.Enabled) || !parseNumber(fields[1], c.Id)) continue;
                    copyField(c.Name, runtimeUnescape(fields[2]));
                    parseNumber(fields[3], c.SourceBindingId);
                    parseNumber(fields[4], condition);
                    c.Condition = static_cast<RuntimeControlCondition>(std::clamp(condition, 0, static_cast<int>(RuntimeControlCondition::FallingEdge)));
                    parseNumber(fields[5], c.ValueA); parseNumber(fields[6], c.ValueB); parseNumber(fields[7], c.Tolerance); parseNumber(fields[8], c.Hysteresis);
                    parseNumber(fields[9], target);
                    c.Target = static_cast<RuntimeControlTarget>(std::clamp(target, 0, static_cast<int>(RuntimeControlTarget::MaterialParameter)));
                    parseNumber(fields[10], c.ShaderPresetIndex); parseNumber(fields[11], c.TargetBindingId); parseNumber(fields[12], c.TargetValue); parseBool(fields[13], c.TargetBool);
                    if (fields.size() > 14) parseNumber(fields[14], c.TargetComponent);
                    if (fields.size() > 15) copyField(c.TargetId, runtimeUnescape(fields[15]));
                    if (fields.size() > 16) parseNumber(fields[16], c.TransitionSeconds);
                    if (c.Id == 0) c.Id = _nextControlId++;
                    else _nextControlId = std::max(_nextControlId, c.Id + 1);
                    _controls.emplace_back(std::move(c));
                }
            }
            validateParameterLinks();
            _savedRevision = _revision;
        }

        static std::string serializeParameterLinks(const RuntimeBinding& binding)
        {
            std::string result;
            for (std::size_t i = 0; i < binding.ParameterLinks.size(); ++i)
            {
                const auto& link = binding.ParameterLinks[i];
                if (!link.Enabled || link.BindingId == 0) continue;
                if (!result.empty()) result.push_back(';');
                result += std::to_string(i) + "=" + std::to_string(link.BindingId);
            }
            return result;
        }

        static void parseParameterLinks(const std::string_view specification, RuntimeBinding& binding)
        {
            std::size_t start = 0;
            while (start < specification.size())
            {
                const std::size_t end = specification.find(';', start);
                const std::string_view token = specification.substr(start, end == std::string_view::npos ? specification.size() - start : end - start);
                const std::size_t equal = token.find('=');
                if (equal != std::string_view::npos)
                {
                    int slot = -1;
                    std::uint64_t id = 0;
                    const auto [slotPtr, slotEc] = std::from_chars(token.data(), token.data() + equal, slot);
                    const auto [idPtr, idEc] = std::from_chars(token.data() + equal + 1, token.data() + token.size(), id);
                    if (slotEc == std::errc{} && idEc == std::errc{} && slotPtr == token.data() + equal && idPtr == token.data() + token.size() && slot >= 0 && slot < static_cast<int>(RuntimeParameterSlot::Count) && id != 0)
                        binding.ParameterLinks[static_cast<std::size_t>(slot)] = {true, id};
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }

        static bool evaluateControlCondition(RuntimeControlRule& control, const float value) noexcept
        {
            const float a = control.ValueA;
            const float b = control.ValueB;
            const float lo = std::min(a, b);
            const float hi = std::max(a, b);
            const float h = std::max(control.Hysteresis, 0.0f);
            const float tolerance = std::max(control.Tolerance, 0.000001f);
            bool active = false;

            switch (control.Condition)
            {
            case RuntimeControlCondition::Equal:
                active = std::abs(value - a) <= tolerance + (control.ConditionActive ? h : 0.0f);
                break;
            case RuntimeControlCondition::NotEqual:
                active = std::abs(value - a) > std::max(tolerance - (control.ConditionActive ? h : 0.0f), 0.000001f);
                break;
            case RuntimeControlCondition::Less:
                active = control.ConditionActive ? value < a + h : value < a;
                break;
            case RuntimeControlCondition::LessEqual:
                active = control.ConditionActive ? value <= a + h : value <= a;
                break;
            case RuntimeControlCondition::Greater:
                active = control.ConditionActive ? value > a - h : value > a;
                break;
            case RuntimeControlCondition::GreaterEqual:
                active = control.ConditionActive ? value >= a - h : value >= a;
                break;
            case RuntimeControlCondition::Between:
                active = control.ConditionActive ? value >= lo - h && value <= hi + h : value >= lo && value <= hi;
                break;
            case RuntimeControlCondition::Outside:
                active = control.ConditionActive ? value < lo + h || value > hi - h : value < lo || value > hi;
                break;
            case RuntimeControlCondition::RisingEdge:
                active = control.PreviousInitialized && control.PreviousValue < a && value >= a;
                break;
            case RuntimeControlCondition::FallingEdge:
                active = control.PreviousInitialized && control.PreviousValue > a && value <= a;
                break;
            }

            control.PreviousValue = value;
            control.PreviousInitialized = true;
            control.ConditionActive = control.Condition == RuntimeControlCondition::RisingEdge || control.Condition == RuntimeControlCondition::FallingEdge ? false : active;
            return active;
        }

        bool bindingDependsOn(const std::uint64_t startId, const std::uint64_t wantedId, std::set<std::uint64_t>& visited) const
        {
            if (startId == wantedId) return true;
            if (!visited.insert(startId).second) return false;
            const RuntimeBinding* binding = findBinding(startId);
            if (!binding) return false;
            for (const auto& link : binding->ParameterLinks)
                if (link.Enabled && link.BindingId != 0 && bindingDependsOn(link.BindingId, wantedId, visited)) return true;
            return false;
        }

        void validateParameterLinks()
        {
            for (auto& binding : _bindings)
            {
                for (auto& link : binding.ParameterLinks)
                {
                    if (!link.Enabled || link.BindingId == 0) continue;
                    std::set<std::uint64_t> visited;
                    if (!findBinding(link.BindingId) || link.BindingId == binding.Id || bindingDependsOn(link.BindingId, binding.Id, visited)) link = {};
                }
            }
        }

        bool readSource(RuntimeBinding& binding, const RuntimeSignalContext& context, float& output)
        {
            const auto signal = std::max(binding.Signal, 0);
            switch (binding.Source)
            {
            case RuntimeSourceKind::Constant:
                output = binding.Constant;
                return true;
            case RuntimeSourceKind::Time:
                if (signal == 1) output = std::sin(static_cast<float>(context.Time));
                else if (signal == 2) output = std::cos(static_cast<float>(context.Time));
                else if (signal == 3) output = static_cast<float>(context.Time - std::floor(context.Time));
                else output = static_cast<float>(context.Time);
                return true;
            case RuntimeSourceKind::Audio:
                if (signal == 1) output = context.Audio.Peak;
                else if (signal == 2) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin(), context.SmoothedBands->begin() + 4, 0.0f) / 4.0f : 0.0f;
                else if (signal == 3) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin() + 4, context.SmoothedBands->begin() + 11, 0.0f) / 7.0f : 0.0f;
                else if (signal == 4) output = context.SmoothedBands ? std::accumulate(context.SmoothedBands->begin() + 11, context.SmoothedBands->end(), 0.0f) / 5.0f : 0.0f;
                else if (signal == 5) output = context.EffectiveGain;
                else if (signal == 6) output = context.GainCorrection;
                else output = context.Audio.Rms;
                return true;
            case RuntimeSourceKind::Media:
            {
                const Color32 color = context.MediaColor.value_or(Color32{0, 0, 0});
                if (signal == 1) output = color.R / 255.0f;
                else if (signal == 2) output = color.G / 255.0f;
                else if (signal == 3) output = color.B / 255.0f;
                else if (signal == 4) output = context.MediaPlaying ? 1.0f : 0.0f;
                else output = context.MediaAmount;
                return true;
            }
            case RuntimeSourceKind::Keyboard:
                if (signal == 0) output = context.Keys.CapsLockActive ? 1.0f : 0.0f;
                else if (signal == 1) output = context.Keys.ScrollLockActive ? 1.0f : 0.0f;
                else if (signal == 2) output = std::accumulate(context.Keys.Down.begin(), context.Keys.Down.end(), 0.0f) / static_cast<float>(MatrixSize);
                else
                {
                    float pulse = 0.0f;
                    for (const auto& event : context.Keys.Events) if (event.Valid > 0.5f) pulse = std::max(pulse, std::exp(-std::max(static_cast<float>(context.Time) - event.Time, 0.0f) * 6.0f));
                    output = pulse;
                }
                return true;
            case RuntimeSourceKind::RPC:
                if (!context.HasPerformance || context.Performance.CoreClock == 0) { binding.Error = "no firmware performance data"; return false; }
                if (signal == 1) output = context.Performance.AverageScanPeriodTicks ? static_cast<float>(context.Performance.CoreClock) / context.Performance.AverageScanPeriodTicks : 0.0f;
                else if (signal == 2) output = (context.Performance.BeginScanTicks + context.Performance.ScanTicks + context.Performance.EndScanTicks) * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 3) output = context.Performance.RGBTicks * 1'000'000.0f / context.Performance.CoreClock;
                else if (signal == 4) output = context.Performance.AverageScanPeriodTicks * 1'000'000.0f / context.Performance.CoreClock;
                else
                {
                    const std::uint32_t total = context.Performance.BeginScanTicks + context.Performance.ScanTicks + context.Performance.EndScanTicks + context.Performance.StateUpdateTicks + context.Performance.HIDTicks;
                    output = context.Performance.AverageScanPeriodTicks ? total * 100.0f / context.Performance.AverageScanPeriodTicks : 0.0f;
                }
                return true;
            case RuntimeSourceKind::Host:
                output = context.AppCpu;
                return true;
            case RuntimeSourceKind::USB:
                if (signal == 0) output = context.USBConnected ? 1.0f : 0.0f;
                else if (signal == 1) output = static_cast<float>(context.USBRates.TxKiB);
                else if (signal == 2) output = static_cast<float>(context.USBRates.RxKiB);
                else if (signal == 3) output = static_cast<float>(context.USBRates.TxTransfers);
                else if (signal == 4) output = static_cast<float>(context.USBRates.RxTransfers);
                else output = static_cast<float>(context.USB.TxErrors + context.USB.RxErrors);
                return true;
            case RuntimeSourceKind::RGB:
            {
                if (!context.Framebuffer) { binding.Error = "no framebuffer"; return false; }
                float r = 0.0f, g = 0.0f, b = 0.0f, lit = 0.0f;
                for (std::size_t row = 0; row < ActiveProbeRows; ++row)
                    for (std::size_t column = 0; column < Columns; ++column)
                    {
                        const auto& color = (*context.Framebuffer)[row * Columns + column];
                        r += color.R; g += color.G; b += color.B;
                        if (color.R || color.G || color.B) lit += 1.0f;
                    }
                constexpr float Count = static_cast<float>(ActiveProbeRows * Columns);
                r /= 255.0f * Count; g /= 255.0f * Count; b /= 255.0f * Count;
                if (signal == 1) output = lit / Count;
                else if (signal == 2) output = r;
                else if (signal == 3) output = g;
                else if (signal == 4) output = b;
                else output = r * 0.2126f + g * 0.7152f + b * 0.0722f;
                return true;
            }
            case RuntimeSourceKind::NativeProcess:
                return readNativeBinding(binding, output);
            case RuntimeSourceKind::BindingStatus:
            {
                RuntimeBinding* target = findBinding(binding.StatusBindingId);
                if (!target || target == &binding) { binding.Error = "status binding target is missing"; return false; }
                if (signal == 0) output = target->HasValue ? 1.0f : 0.0f;
                else if (signal == 1) output = target->LastReadSucceeded ? 1.0f : 0.0f;
                else if (signal == 2) output = target->Enabled && target->RuntimeEnabled ? 1.0f : 0.0f;
                else if (signal == 3) output = target->Source == RuntimeSourceKind::NativeProcess && runtimeProcessIsAlive(static_cast<pid_t>(target->ProcessId)) ? 1.0f : 0.0f;
                else if (signal == 4)
                {
                    if (target->Source != RuntimeSourceKind::NativeProcess) output = target->HasValue ? 1.0f : 0.0f;
                    else if (target->AddressMode == ProcessAddressMode::Signature) output = target->SignatureResolvedAddress != 0 ? 1.0f : 0.0f;
                    else output = target->LastReadSucceeded ? 1.0f : 0.0f;
                }
                else if (signal == 5) output = target->Source == RuntimeSourceKind::NativeProcess && target->AddressMode == ProcessAddressMode::Signature && target->SignatureResolve == SignatureResultMode::RegisterRelativeCapture && target->SignatureResolvedAddress != 0 ? 1.0f : 0.0f;
                else output = target->Error.empty() ? 0.0f : 1.0f;
                binding.Error.clear();
                return true;
            }
            }
            return false;
        }

        std::filesystem::path _path = runtimeBindingsPath();
        std::vector<RuntimeBinding> _bindings;
        std::vector<RuntimeControlRule> _controls;
        std::uint64_t _nextBindingId = 1;
        std::uint64_t _nextControlId = 1;
        std::uint64_t _revision = 0;
        std::uint64_t _savedRevision = 0;
        USBStatsSnapshot _lastUSB{};
        RuntimeUSBRates _usbRates{};
        double _rateTime = 0.0;
    };

    class RuntimeTelemetry
    {
    public:
        void event(const double time, std::string category, std::string text)
        {
            std::lock_guard lock(_mutex);
            _events.push_back({time, std::move(category), std::move(text)});
            while (_events.size() > 512) _events.pop_front();
        }

        void packet(const double time, const bool tx, const PacketHeader& header, const std::span<const std::byte> bytes)
        {
            std::lock_guard lock(_mutex);
            RuntimePacketRecord record;
            record.Time = time;
            record.Tx = tx;
            record.Type = static_cast<std::uint16_t>(header.Type);
            record.Version = header.Version;
            record.PacketId = header.PacketId;
            record.ResponseFor = header.ResponseFor;
            record.PayloadLength = header.PayloadLength;
            record.Bytes = bytes.size();
            record.CapturedBytes = std::min(bytes.size(), record.Data.size());
            std::copy_n(bytes.begin(), record.CapturedBytes, record.Data.begin());
            _packets.push_back(record);
            while (_packets.size() > 512) _packets.pop_front();
        }

        std::deque<RuntimeTimelineEvent> events() const { std::lock_guard lock(_mutex); return _events; }
        std::deque<RuntimePacketRecord> packets() const { std::lock_guard lock(_mutex); return _packets; }
        void clearEvents() { std::lock_guard lock(_mutex); _events.clear(); }
        void clearPackets() { std::lock_guard lock(_mutex); _packets.clear(); }

    private:
        mutable std::mutex _mutex;
        std::deque<RuntimeTimelineEvent> _events;
        std::deque<RuntimePacketRecord> _packets;
    };

    static void mapSpectrumToColumns(const std::span<const float> analysisBands, std::array<float, Columns>& bands, const VisualizerSettings& settings, const float overallGain)
    {
        if (analysisBands.empty())
        {
            bands.fill(0.0f);
            return;
        }
        const int bassColumns = std::clamp(settings.BassColumns, 2, static_cast<int>(Columns) - 2);
        const int bassEndBand = std::clamp(settings.BassEndBand, 0, static_cast<int>(analysisBands.size()) - 1);
        for (int column = 0; column < static_cast<int>(Columns); ++column)
        {
            int sourceBand;
            float level;
            float gain = settings.ColumnGain[column] * overallGain;
            if (column < bassColumns)
            {
                const float t = column / static_cast<float>(bassColumns - 1);
                sourceBand = static_cast<int>(std::lround(t * bassEndBand));
                level = analysisBands[sourceBand];
                float activation = std::clamp((level - settings.BassActivationThreshold) / std::max(0.001f, 1.0f - settings.BassActivationThreshold), 0.0f, 1.0f);
                activation = activation * activation * (3.0f - 2.0f * activation);
                gain *= 1.0f + (settings.BassMaxBoost - 1.0f) * activation;
            }
            else
            {
                const float t = (column - bassColumns) / static_cast<float>(Columns - bassColumns - 1);
                const int firstHighBand = std::min(bassEndBand + 2, static_cast<int>(analysisBands.size()) - 1);
                sourceBand = static_cast<int>(std::lround(firstHighBand + t * (static_cast<int>(analysisBands.size()) - 1 - firstHighBand)));
                level = analysisBands[sourceBand];
            }
            bands[column] = std::clamp(level * gain, 0.0f, 1.0f);
        }
    }

    static void smoothBands(const std::array<float, Columns>& bands, std::array<float, Columns>& smoothedBands, const VisualizerSettings& settings, const float dt)
    {
        for (std::size_t i = 0; i < Columns; ++i)
        {
            const float target = bands[i];
            const float speed = target > smoothedBands[i] ? settings.AttackSpeed : settings.ReleaseSpeed;
            const float alpha = 1.0f - std::exp(-speed * dt);
            smoothedBands[i] += (target - smoothedBands[i]) * alpha;
        }
    }

    static void renderAudioRGB(std::array<Color32, MatrixSize>& framebuffer, const std::array<float, Columns>& bands, const VisualizerSettings& settings, const std::optional<Color32> visualizerColor, const float mediaColorAmount, const float wavePhase)
    {
        framebuffer.fill({0, 0, 0});
        constexpr int VisibleRows = static_cast<int>(Rows) - 1;
        const Color32 solid = floatColor(settings.SolidColor);
        for (std::size_t column = 0; column < Columns; ++column)
        {
            const float level = std::clamp(bands[column], 0.0f, 1.0f);
            const float exactRows = level * VisibleRows;
            const Color32 waveColor = hsvToRgb(static_cast<float>(column) / static_cast<float>(Columns) - wavePhase, 1.0f, 1.0f);
            Color32 baseColor = settings.BaseColorMode == 0 ? waveColor : solid;
            if (visualizerColor)
                baseColor = lerpColor(baseColor, *visualizerColor, mediaColorAmount * settings.MediaColorBlend);
            for (int visualRow = 0; visualRow < VisibleRows; ++visualRow)
            {
                // The physical LED rows are framebuffer rows 0..5. Row 6 is the unused extra row.
                const int row = VisibleRows - 1 - visualRow;
                float amount = settings.ForceFullRow && row == settings.FullRow ? 1.0f : std::clamp((exactRows - visualRow) / settings.FeatherRows, 0.0f, 1.0f);
                amount = amount * amount * (3.0f - 2.0f * amount);
                float r = baseColor.R * amount;
                float g = baseColor.G * amount;
                float b = baseColor.B * amount;
                saturate(r, g, b, settings.Saturation);
                framebuffer[row * Columns + column] = {
                    static_cast<std::uint8_t>(std::lround(r)),
                    static_cast<std::uint8_t>(std::lround(g)),
                    static_cast<std::uint8_t>(std::lround(b))
                };
            }
        }
    }

    static bool sendFramebuffer(RawUSB& usb, const std::array<Color32, MatrixSize>& framebuffer)
    {
        FramebufferSetPayload<MatrixSize> payload{};
        payload.Framebuffer = framebuffer;
        return usb.send(makePacket(PacketType::FramebufferSet, payload));
    }

    static void applyGlobalBrightness(std::array<Color32, MatrixSize>& framebuffer, const float brightness) noexcept
    {
        const float amount = std::clamp(brightness, 0.0f, 1.0f);
        if (amount >= 0.9999f) return;
        for (auto& color : framebuffer)
        {
            color.R = static_cast<std::uint8_t>(std::lround(color.R * amount));
            color.G = static_cast<std::uint8_t>(std::lround(color.G * amount));
            color.B = static_cast<std::uint8_t>(std::lround(color.B * amount));
        }
    }

    struct PreviewRect
    {
        std::uint8_t Row;
        std::uint8_t Column;
        float X;
        float Y;
        float Width;
        float Height;
    };

    static Color32 interpolatePreviewColor(const std::array<Color32, MatrixSize>& framebuffer, const std::uint8_t row, const std::uint8_t column, const float interpolation) noexcept
    {
        const Color32 center = framebuffer[static_cast<std::size_t>(row) * Columns + column];
        const float amount = std::clamp(interpolation, 0.0f, 1.0f);
        if (amount <= 0.0001f) return center;

        float r = 0.0f, g = 0.0f, b = 0.0f, totalWeight = 0.0f;
        for (int y = -1; y <= 1; ++y)
        {
            const int sampleRow = static_cast<int>(row) + y;
            if (sampleRow < 0 || sampleRow >= static_cast<int>(ActiveProbeRows)) continue;
            for (int x = -1; x <= 1; ++x)
            {
                const int sampleColumn = static_cast<int>(column) + x;
                if (sampleColumn < 0 || sampleColumn >= static_cast<int>(Columns)) continue;
                const float weight = (x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f);
                const Color32 sample = framebuffer[static_cast<std::size_t>(sampleRow) * Columns + static_cast<std::size_t>(sampleColumn)];
                r += sample.R * weight;
                g += sample.G * weight;
                b += sample.B * weight;
                totalWeight += weight;
            }
        }
        if (totalWeight <= 0.0f) return center;
        const auto mixChannel = [amount, totalWeight](const std::uint8_t source, const float accumulated) -> std::uint8_t
        {
            return static_cast<std::uint8_t>(std::lround(std::clamp(source + (accumulated / totalWeight - source) * amount, 0.0f, 255.0f)));
        };
        return {mixChannel(center.R, r), mixChannel(center.G, g), mixChannel(center.B, b)};
    }

    static void drawFramebufferPreview(const std::array<Color32, MatrixSize>& framebuffer, const float widthFraction = 0.55f, const float maxWidth = 532.0f, const float interpolation = 0.0f)
    {
        // Approximate physical K552 ISO/TKL silhouette. Framebuffer rows 0..5 are the
        // six visible keyboard rows; row 6 is unused. Wide physical keys reuse the
        // nearest framebuffer column color because the framebuffer itself is still 7x16.
        static constexpr auto Keys = std::to_array<PreviewRect>({
            // Function row
            {0,  0,  0.00f, 0.00f, 1.00f, 0.95f},
            {0,  1,  2.00f, 0.00f, 1.00f, 0.95f}, {0,  2,  3.00f, 0.00f, 1.00f, 0.95f}, {0,  3,  4.00f, 0.00f, 1.00f, 0.95f}, {0,  4,  5.00f, 0.00f, 1.00f, 0.95f},
            {0,  5,  6.50f, 0.00f, 1.00f, 0.95f}, {0,  6,  7.50f, 0.00f, 1.00f, 0.95f}, {0,  7,  8.50f, 0.00f, 1.00f, 0.95f}, {0,  8,  9.50f, 0.00f, 1.00f, 0.95f},
            {0,  9, 11.00f, 0.00f, 1.00f, 0.95f}, {0, 10, 12.00f, 0.00f, 1.00f, 0.95f}, {0, 11, 13.00f, 0.00f, 1.00f, 0.95f}, {0, 12, 14.00f, 0.00f, 1.00f, 0.95f},
            {0, 13, 16.00f, 0.00f, 1.00f, 0.95f}, {0, 14, 17.00f, 0.00f, 1.00f, 0.95f}, {0, 15, 18.00f, 0.00f, 1.00f, 0.95f},

            // Number row: ` 1..0 - = Backspace | Ins Home PgUp
            {1,  0,  0.00f, 1.35f, 1.00f, 0.95f}, {1,  1,  1.00f, 1.35f, 1.00f, 0.95f}, {1,  2,  2.00f, 1.35f, 1.00f, 0.95f}, {1,  3,  3.00f, 1.35f, 1.00f, 0.95f},
            {1,  4,  4.00f, 1.35f, 1.00f, 0.95f}, {1,  5,  5.00f, 1.35f, 1.00f, 0.95f}, {1,  6,  6.00f, 1.35f, 1.00f, 0.95f}, {1,  7,  7.00f, 1.35f, 1.00f, 0.95f},
            {1,  8,  8.00f, 1.35f, 1.00f, 0.95f}, {1,  9,  9.00f, 1.35f, 1.00f, 0.95f}, {1, 10, 10.00f, 1.35f, 1.00f, 0.95f}, {1, 11, 11.00f, 1.35f, 1.00f, 0.95f},
            {1, 12, 12.00f, 1.35f, 1.00f, 0.95f}, {1, 12, 13.00f, 1.35f, 2.00f, 0.95f},
            {1, 13, 16.00f, 1.35f, 1.00f, 0.95f}, {1, 14, 17.00f, 1.35f, 1.00f, 0.95f}, {1, 15, 18.00f, 1.35f, 1.00f, 0.95f},

            // QWERTY row: Tab + 12-ish 1u keys + top of ISO Enter | Del End PgDn
            {2,  0,  0.00f, 2.35f, 1.50f, 0.95f},
            {2,  1,  1.50f, 2.35f, 1.00f, 0.95f}, {2,  2,  2.50f, 2.35f, 1.00f, 0.95f}, {2,  3,  3.50f, 2.35f, 1.00f, 0.95f}, {2,  4,  4.50f, 2.35f, 1.00f, 0.95f},
            {2,  5,  5.50f, 2.35f, 1.00f, 0.95f}, {2,  6,  6.50f, 2.35f, 1.00f, 0.95f}, {2,  7,  7.50f, 2.35f, 1.00f, 0.95f}, {2,  8,  8.50f, 2.35f, 1.00f, 0.95f},
            {2,  9,  9.50f, 2.35f, 1.00f, 0.95f}, {2, 10, 10.50f, 2.35f, 1.00f, 0.95f}, {2, 11, 11.50f, 2.35f, 1.00f, 0.95f}, {2, 12, 12.50f, 2.35f, 1.00f, 0.95f},
            {2, 12, 13.50f, 2.35f, 1.50f, 0.95f},
            {2, 13, 16.00f, 2.35f, 1.00f, 0.95f}, {2, 14, 17.00f, 2.35f, 1.00f, 0.95f}, {2, 15, 18.00f, 2.35f, 1.00f, 0.95f},

            // Home row: Caps + letters/punctuation + lower part of ISO Enter
            {3,  0,  0.00f, 3.35f, 1.75f, 0.95f},
            {3,  1,  1.75f, 3.35f, 1.00f, 0.95f}, {3,  2,  2.75f, 3.35f, 1.00f, 0.95f}, {3,  3,  3.75f, 3.35f, 1.00f, 0.95f}, {3,  4,  4.75f, 3.35f, 1.00f, 0.95f},
            {3,  5,  5.75f, 3.35f, 1.00f, 0.95f}, {3,  6,  6.75f, 3.35f, 1.00f, 0.95f}, {3,  7,  7.75f, 3.35f, 1.00f, 0.95f}, {3,  8,  8.75f, 3.35f, 1.00f, 0.95f},
            {3,  9,  9.75f, 3.35f, 1.00f, 0.95f}, {3, 10, 10.75f, 3.35f, 1.00f, 0.95f}, {3, 11, 11.75f, 3.35f, 1.00f, 0.95f}, {3, 12, 12.75f, 3.35f, 1.00f, 0.95f},
            {3, 13, 13.75f, 3.35f, 1.25f, 0.95f},

            // Shift row + Up
            {4,  0,  0.00f, 4.35f, 1.25f, 0.95f}, {4,  1,  1.25f, 4.35f, 1.00f, 0.95f},
            {4,  2,  2.25f, 4.35f, 1.00f, 0.95f}, {4,  3,  3.25f, 4.35f, 1.00f, 0.95f}, {4,  4,  4.25f, 4.35f, 1.00f, 0.95f}, {4,  5,  5.25f, 4.35f, 1.00f, 0.95f},
            {4,  6,  6.25f, 4.35f, 1.00f, 0.95f}, {4,  7,  7.25f, 4.35f, 1.00f, 0.95f}, {4,  8,  8.25f, 4.35f, 1.00f, 0.95f}, {4,  9,  9.25f, 4.35f, 1.00f, 0.95f},
            {4, 10, 10.25f, 4.35f, 1.00f, 0.95f}, {4, 11, 11.25f, 4.35f, 1.00f, 0.95f}, {4, 12, 12.25f, 4.35f, 1.00f, 0.95f},
            {4, 15, 13.25f, 4.35f, 1.75f, 0.95f}, {4, 14, 17.00f, 4.35f, 1.00f, 0.95f},

            // Bottom row + arrows
            {5,  0,  0.00f, 5.35f, 1.25f, 0.95f}, {5,  1,  1.25f, 5.35f, 1.25f, 0.95f}, {5,  2,  2.50f, 5.35f, 1.25f, 0.95f},
            {5,  6,  3.75f, 5.35f, 6.25f, 0.95f},
            {5,  9, 10.00f, 5.35f, 1.25f, 0.95f}, {5, 10, 11.25f, 5.35f, 1.25f, 0.95f}, {5, 11, 12.50f, 5.35f, 1.25f, 0.95f}, {5, 12, 13.75f, 5.35f, 1.25f, 0.95f},
            {5, 13, 16.00f, 5.35f, 1.00f, 0.95f}, {5, 14, 17.00f, 5.35f, 1.00f, 0.95f}, {5, 15, 18.00f, 5.35f, 1.00f, 0.95f}
        });

        constexpr float LayoutWidth = 19.00f;
        constexpr float LayoutHeight = 6.35f;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float previewWidth = std::min(availableWidth * widthFraction, maxWidth);
        const float unit = std::clamp((previewWidth - 2.0f) / LayoutWidth, 7.0f, 28.0f);
        const float gap = unit * 0.045f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& key : Keys)
        {
            const Color32 color = interpolatePreviewColor(framebuffer, key.Row, key.Column, interpolation);
            // Preview interpolation is visual-only: it approximates light bleed/color mixing between
            // neighboring switches without touching the framebuffer actually sent to the keyboard.
            const ImU32 previewColor = color.R == 0 && color.G == 0 && color.B == 0 ? IM_COL32(24, 24, 24, 255) : IM_COL32(color.R, color.G, color.B, 255);
            const ImVec2 min(origin.x + key.X * unit + gap, origin.y + key.Y * unit + gap);
            const ImVec2 max(origin.x + (key.X + key.Width) * unit - gap, origin.y + (key.Y + key.Height) * unit - gap);
            drawList->AddRectFilled(min, max, previewColor, 0.0f);
        }

        ImGui::Dummy(ImVec2(LayoutWidth * unit, LayoutHeight * unit));
    }

    static void drawShaderLivePanel(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive, VisualizerSettings& settings)
    {
        PerformanceSnapshot performance{};
        bool hasPerformance = false;
        {
            std::lock_guard lock(deviceState.Mutex);
            performance = deviceState.Performance;
            hasPerformance = deviceState.HasPerformance;
        }

        ImGui::SeparatorText("Live output");
        if (ImGui::BeginTable("ShaderLiveOutput", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 430.0f);
            ImGui::TableSetupColumn("Diagnostics", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            drawFramebufferPreview(framebuffer, 1.0f, 410.0f, settings.LiveOutputInterpolation);
            ImGui::SetNextItemWidth(410.0f);
            ImGui::SliderFloat("Live output interpolation##ShaderLive", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview-only neighboring-key color mixing; USB framebuffer data is unchanged.");

            ImGui::TableNextColumn();
            ImGui::Text("Current shader: %s", settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str());
            ImGui::Text("Firmware: %s   QRPC: v%u", FirmwareVersion, static_cast<unsigned>(ProtocolVersion));
            ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
            ImGui::Text("Global brightness: %.0f%%   Preview interpolation: %.0f%%", settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);
            ImGui::Text("Framebuffer: %zux%zu (%zu cells, %zu active RGB rows)", Columns, Rows, MatrixSize, ActiveProbeRows);
            ImGui::TextUnformatted("Device");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", usb.isConnected() ? (usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str()) : "Disconnected");
            if (keyboardInput.connected() && !keyboardInput.deviceName().empty())
                ImGui::Text("Input device: %s", keyboardInput.deviceName().c_str());
            ImGui::TextWrapped("Input: %s", keyboardInput.status().c_str());
            ImGui::Text("App CPU: %.2f%%", appCpuUsage);
            ImGui::Text("evdev: Caps %.0f   Scroll %.0f", capsLockActive ? 1.0f : 0.0f, scrollLockActive ? 1.0f : 0.0f);
            ImGui::TextDisabled("Shader: uCapsLock %.0f   uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);

            if (!hasPerformance || performance.CoreClock == 0)
                ImGui::TextDisabled("Waiting for keyboard performance data...");
            else
            {
                const double ticksPerMicrosecond = performance.CoreClock / 1'000'000.0;
                const std::uint32_t matrixTicks = performance.BeginScanTicks + performance.ScanTicks + performance.EndScanTicks;
                const std::uint32_t totalTicks = matrixTicks + performance.StateUpdateTicks + performance.HIDTicks;
                const double keyboardCpu = performance.AverageScanPeriodTicks != 0 ? static_cast<double>(totalTicks) / performance.AverageScanPeriodTicks * 100.0 : 0.0;
                const double scanRate = performance.AverageScanPeriodTicks != 0 ? static_cast<double>(performance.CoreClock) / performance.AverageScanPeriodTicks : 0.0;
                ImGui::Text("Keyboard CPU: %.2f%%   Scan: %.1f Hz", keyboardCpu, scanRate);
                ImGui::Text("Matrix %.2f us  [begin %.2f / scan %.2f / end %.2f]", matrixTicks / ticksPerMicrosecond, performance.BeginScanTicks / ticksPerMicrosecond, performance.ScanTicks / ticksPerMicrosecond, performance.EndScanTicks / ticksPerMicrosecond);
                ImGui::Text("State %.2f us   HID %.2f us", performance.StateUpdateTicks / ticksPerMicrosecond, performance.HIDTicks / ticksPerMicrosecond);
                ImGui::Text("RGB %.2f us   slot max %.2f us   period %.2f us", performance.RGBTicks / ticksPerMicrosecond, performance.RGBSlotMaxTicks / ticksPerMicrosecond, performance.AverageScanPeriodTicks / ticksPerMicrosecond);
            }
            ImGui::EndTable();
        }
    }

    static void drawPerformance(const PerformanceSnapshot& stats)
    {
        if (stats.CoreClock == 0)
        {
            ImGui::TextDisabled("No performance response yet.");
            return;
        }
        const double ticksPerMicrosecond = stats.CoreClock / 1'000'000.0;
        const std::uint32_t matrixTicks = stats.BeginScanTicks + stats.ScanTicks + stats.EndScanTicks;
        const std::uint32_t totalTicks = matrixTicks + stats.StateUpdateTicks + stats.HIDTicks;
        const double cpuUsage = stats.AverageScanPeriodTicks != 0 ? static_cast<double>(totalTicks) / stats.AverageScanPeriodTicks * 100.0 : 0.0;
        const double scanRate = stats.AverageScanPeriodTicks != 0 ? static_cast<double>(stats.CoreClock) / stats.AverageScanPeriodTicks : 0.0;
        ImGui::Text("Core clock %.2f MHz", stats.CoreClock / 1'000'000.0);
        ImGui::Text("Scan rate %.2f Hz", scanRate);
        ImGui::Text("CPU %.2f%%", cpuUsage);
        ImGui::ProgressBar(static_cast<float>(std::clamp(cpuUsage / 100.0, 0.0, 1.0)), ImVec2(-1.0f, 0.0f));
        if (ImGui::BeginTable("PerformanceTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Stage");
            ImGui::TableSetupColumn("Ticks");
            ImGui::TableSetupColumn("us");
            ImGui::TableHeadersRow();
            const auto row = [&](const char* name, const std::uint32_t ticks)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                ImGui::TableNextColumn(); ImGui::Text("%u", ticks);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", ticks / ticksPerMicrosecond);
            };
            row("Begin", stats.BeginScanTicks);
            row("Scan", stats.ScanTicks);
            row("End", stats.EndScanTicks);
            row("State", stats.StateUpdateTicks);
            row("HID", stats.HIDTicks);
            row("RGB driver", stats.RGBTicks);
            row("RGB slot max", stats.RGBSlotMaxTicks);
            row("Total", totalTicks);
            row("Period", stats.AverageScanPeriodTicks);
            ImGui::EndTable();
        }
    }

    static void drawTimingProbe(const MatrixTimingProbeResult<ActiveProbeRows>& probe)
    {
        if (probe.CoreClock == 0)
        {
            ImGui::TextDisabled("No matrix timing probe result yet.");
            return;
        }
        const double ticksPerMicrosecond = probe.CoreClock / 1'000'000.0;
        if (ImGui::BeginTable("TimingProbe", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Row");
            ImGui::TableSetupColumn("Col");
            ImGui::TableSetupColumn("Min us");
            ImGui::TableSetupColumn("Max us");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Timeouts");
            ImGui::TableSetupColumn("Suggested us");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < ActiveProbeRows; ++i)
            {
                const auto& result = probe.Rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
                if (result.Column == 0xFF || result.Samples == 0)
                {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    for (int column = 2; column < 7; ++column) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                    continue;
                }
                const double minUs = result.MinTicks / ticksPerMicrosecond;
                const double maxUs = result.MaxTicks / ticksPerMicrosecond;
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Column);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", minUs);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", maxUs);
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Samples);
                ImGui::TableNextColumn(); ImGui::Text("%u", result.Timeouts);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", maxUs + 5.0);
            }
            ImGui::EndTable();
        }
    }


    static const char* packetTypeLabel(const std::uint16_t type)
    {
        if (type == static_cast<std::uint16_t>(PacketType::FramebufferSet)) return "FramebufferSet";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceRequest)) return "PerformanceRequest";
        if (type == static_cast<std::uint16_t>(PacketType::PerformanceResponse)) return "PerformanceResponse";
        if (type == static_cast<std::uint16_t>(PacketType::MatrixTimingProbeResult)) return "MatrixTimingProbeResult";
        return "Other";
    }

    static void selectRuntimeProcess(RuntimeBinding& binding, const RuntimeProcessInfo& process)
    {
        binding.ProcessId = static_cast<int>(process.Pid);
        std::snprintf(binding.ProcessName, sizeof(binding.ProcessName), "%s", process.Name.c_str());
        binding.NextProcessSearch = 0.0;
        captureRuntimeRebindPattern(binding, process);
    }

    static bool drawRuntimeTargetSelector(RuntimeBinding& binding, ShaderFramebuffer& shaderFramebuffer)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Uniform id", binding.TargetId[0] ? binding.TargetId : "<select material uniform>"))
        {
            for (const auto& parameter : shaderFramebuffer.materialParameters())
            {
                const bool selected = parameter.PersistenceKey == binding.TargetId;
                const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", parameter.PersistenceKey.c_str());
                    binding.TargetComponent = std::clamp(binding.TargetComponent, 0, std::max(parameter.Components - 1, 0));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (const auto* parameter = shaderFramebuffer.findMaterialParameter(binding.TargetId))
        {
            if (parameter->Components > 1)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                changed |= ImGui::SliderInt("Component", &binding.TargetComponent, 0, parameter->Components - 1);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s / %s", parameter->Label.c_str(), parameter->Integer ? (parameter->Boolean ? "bool" : "int") : "float");
        }
        else
            ImGui::TextDisabled("Current shader does not expose this material id; the binding will wait.");
        return changed;
    }

    static bool drawRuntimeProcessSelector(RuntimeBinding& binding, std::vector<RuntimeProcessInfo>& processes)
    {
        bool changed = false;
        if (ImGui::SmallButton("Refresh processes")) processes = enumerateRuntimeProcesses();
        ImGui::SameLine();
        const std::string loweredSearch = runtimeLower(binding.ProcessSearch);
        const std::size_t matching = static_cast<std::size_t>(std::ranges::count_if(processes, [&](const RuntimeProcessInfo& process) { return runtimeProcessMatchesSearch(process, loweredSearch); }));
        ImGui::TextDisabled("%zu processes / %zu matching", processes.size(), matching);

        const auto selectedIt = std::ranges::find_if(processes, [&](const RuntimeProcessInfo& process) { return process.Pid == binding.ProcessId; });
        const std::string current = selectedIt != processes.end() ? std::to_string(selectedIt->Pid) + "  " + runtimeProcessDisplayTitle(*selectedIt) : binding.ProcessId > 0 ? std::to_string(binding.ProcessId) + "  " + binding.ProcessName : "<select process>";
        ImGui::SetNextItemWidth(690.0f);
        if (ImGui::BeginCombo("Process", current.c_str(), ImGuiComboFlags_HeightLargest))
        {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##ProcessSearch", "Find PID, process name, title/argv[0], executable path or command line...", binding.ProcessSearch, sizeof(binding.ProcessSearch));
            ImGui::Separator();
            if (ImGui::BeginTable("ProcessPicker", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(780.0f, 330.0f)))
            {
                ImGui::TableSetupColumn("Process / title", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableSetupColumn("Executable / command", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                ImGui::TableHeadersRow();
                for (const auto& process : processes)
                {
                    if (!runtimeProcessMatchesSearch(process, loweredSearch)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string display = runtimeProcessDisplayTitle(process) + "##Process" + std::to_string(process.Pid);
                    if (ImGui::Selectable(display.c_str(), process.Pid == binding.ProcessId))
                    {
                        selectRuntimeProcess(binding, process);
                        changed = true;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("PID: %d", static_cast<int>(process.Pid));
                        ImGui::Text("Name: %s", process.Name.c_str());
                        ImGui::TextWrapped("Title / argv[0]: %s", process.Title.c_str());
                        ImGui::TextWrapped("Executable: %s", process.Exe.c_str());
                        ImGui::TextWrapped("Command line: %s", process.CommandLine.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(process.Pid));
                    ImGui::TableNextColumn();
                    if (!process.Exe.empty()) ImGui::TextUnformatted(process.Exe.c_str());
                    if (!process.CommandLine.empty() && process.CommandLine != process.Exe)
                    {
                        if (!process.Exe.empty()) ImGui::TextDisabled("%s", process.CommandLine.c_str());
                        else ImGui::TextUnformatted(process.CommandLine.c_str());
                    }
                }
                ImGui::EndTable();
            }
            if (matching == 0) ImGui::TextDisabled("No process matches '%s'.", binding.ProcessSearch);
            ImGui::EndCombo();
        }

        changed |= ImGui::Checkbox("Auto rebind when process restarts", &binding.AutoReattach);
        if (binding.AutoReattach)
        {
            int mode = static_cast<int>(binding.RebindMode);
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("Rebind using", runtimeRebindModeName(binding.RebindMode)))
            {
                for (int i = 0; i <= static_cast<int>(ProcessRebindMode::AnyRegex); ++i)
                {
                    const auto candidate = static_cast<ProcessRebindMode>(i);
                    if (ImGui::Selectable(runtimeRebindModeName(candidate), mode == i))
                    {
                        binding.RebindMode = candidate;
                        mode = i;
                        const auto selected = std::ranges::find_if(processes, [&](const RuntimeProcessInfo& process) { return process.Pid == binding.ProcessId; });
                        if (selected != processes.end()) captureRuntimeRebindPattern(binding, *selected);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(690.0f);
            changed |= ImGui::InputText(runtimeRebindModeIsRegex(binding.RebindMode) ? "Rebind regex" : "Rebind value", binding.ProcessRebindPattern, sizeof(binding.ProcessRebindPattern));
            if (runtimeRebindModeIsRegex(binding.RebindMode) && binding.ProcessRebindPattern[0])
            {
                try { [[maybe_unused]] const std::regex test(binding.ProcessRebindPattern, std::regex::ECMAScript | std::regex::icase); ImGui::SameLine(); ImGui::TextDisabled("regex OK"); }
                catch (const std::regex_error& e) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "invalid regex: %s", e.what()); }
            }
            ImGui::TextDisabled("If the PID disappears Quartz searches about once per second and rebinds to the newest matching process.");
        }
        return changed;
    }

    static const char* runtimeParameterSlotName(const RuntimeParameterSlot slot) noexcept
    {
        switch (slot)
        {
        case RuntimeParameterSlot::Normalize: return "Normalize";
        case RuntimeParameterSlot::InputMin: return "Input min";
        case RuntimeParameterSlot::InputMax: return "Input max";
        case RuntimeParameterSlot::Invert: return "Invert";
        case RuntimeParameterSlot::Scale: return "Scale";
        case RuntimeParameterSlot::Offset: return "Offset";
        case RuntimeParameterSlot::Clamp: return "Clamp";
        case RuntimeParameterSlot::OutputMin: return "Output min";
        case RuntimeParameterSlot::OutputMax: return "Output max";
        case RuntimeParameterSlot::SmoothingHz: return "Smoothing";
        case RuntimeParameterSlot::UpdateHz: return "Update rate";
        case RuntimeParameterSlot::Count: break;
        }
        return "Parameter";
    }

    static bool drawRuntimeParameterLink(RuntimeBindingEngine& engine, RuntimeBinding& owner, const RuntimeParameterSlot slot)
    {
        bool changed = false;
        auto& link = owner.ParameterLinks[static_cast<std::size_t>(slot)];
        std::string preview = "Local";
        if (link.Enabled)
        {
            if (const auto* source = engine.findBinding(link.BindingId))
            {
                preview = source->Name[0] ? std::string(source->Name) : ("Binding " + std::to_string(source->Id));
                if (source->HasValue)
                {
                    char value[48];
                    std::snprintf(value, sizeof(value), " = %.4g", source->Value);
                    preview += value;
                }
            }
            else preview = "Missing binding";
        }
        ImGui::PushID(static_cast<int>(slot));
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##ParameterSource", preview.c_str()))
        {
            if (ImGui::Selectable("Local value", !link.Enabled))
            {
                engine.setParameterLink(owner, slot, 0);
                changed = true;
            }
            for (const auto& candidate : engine.bindings())
            {
                if (candidate.Id == owner.Id) continue;
                const bool selected = link.Enabled && link.BindingId == candidate.Id;
                const bool allowed = selected || engine.canParameterLink(owner.Id, candidate.Id);
                if (!allowed) ImGui::BeginDisabled();
                std::string label = std::string(candidate.Name[0] ? candidate.Name : "Binding") + "  [#" + std::to_string(candidate.Id) + "]";
                if (ImGui::Selectable(label.c_str(), selected) && allowed)
                {
                    engine.setParameterLink(owner, slot, candidate.Id);
                    changed = true;
                }
                if (!allowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Blocked because this dependency would create a circular binding loop.");
                if (!allowed) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use another binding's output as %s. Local value is used until the linked binding has produced a value.", runtimeParameterSlotName(slot));
        ImGui::PopID();
        return changed;
    }

    static bool drawRuntimeLinkedFloat(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, float& value, const float speed = 0.01f, const char* format = "%.3f")
    {
        bool changed = false;
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::DragFloat(label, &value, speed, 0.0f, 0.0f, format);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    static bool drawRuntimeLinkedBool(RuntimeBindingEngine& engine, RuntimeBinding& binding, const RuntimeParameterSlot slot, const char* label, bool& value)
    {
        bool changed = ImGui::Checkbox(label, &value);
        ImGui::SameLine();
        changed |= drawRuntimeParameterLink(engine, binding, slot);
        return changed;
    }

    static bool drawRuntimeBinding(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeBinding& binding, const std::size_t index, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeBinding");
        ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
        const std::string bindingHeader = std::string(binding.Name[0] ? binding.Name : "Binding") + "###RuntimeBinding" + std::to_string(binding.Id);
        if (!ImGui::CollapsingHeader(bindingHeader.c_str()))
        {
            ImGui::PopID();
            ImGui::PopID();
            return false;
        }
        changed |= ImGui::Checkbox("Enabled", &binding.Enabled);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        changed |= ImGui::InputText("Name", binding.Name, sizeof(binding.Name));

        int source = static_cast<int>(binding.Source);
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::BeginCombo("Source", runtimeSourceName(binding.Source)))
        {
            for (int i = 0; i <= static_cast<int>(RuntimeSourceKind::BindingStatus); ++i)
            {
                const auto candidate = static_cast<RuntimeSourceKind>(i);
                if (ImGui::Selectable(runtimeSourceName(candidate), i == source))
                {
                    binding.Source = candidate;
                    binding.Signal = 0;
                    source = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        const auto signals = runtimeSignalNames(binding.Source);
        binding.Signal = std::clamp(binding.Signal, 0, std::max(static_cast<int>(signals.size()) - 1, 0));
        if (signals.size() > 1)
        {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("Signal", signals[static_cast<std::size_t>(binding.Signal)].data()))
            {
                for (int i = 0; i < static_cast<int>(signals.size()); ++i)
                    if (ImGui::Selectable(signals[static_cast<std::size_t>(i)].data(), binding.Signal == i)) { binding.Signal = i; changed = true; }
                ImGui::EndCombo();
            }
        }

        if (binding.Source == RuntimeSourceKind::Constant)
            changed |= ImGui::DragFloat("Constant value", &binding.Constant, 0.01f);

        static std::vector<RuntimeProcessInfo> processes;
        if (binding.Source == RuntimeSourceKind::NativeProcess && processes.empty())
            processes = enumerateRuntimeProcesses();

        if (binding.Source == RuntimeSourceKind::NativeProcess)
        {
            ImGui::SeparatorText("Native process memory");
            changed |= drawRuntimeProcessSelector(binding, processes);
            static std::vector<RuntimeProcessModule> modules;
            static int modulePid = -1;
            if (ImGui::SmallButton("Refresh modules") || modulePid != binding.ProcessId)
            {
                modules = binding.ProcessId > 0 ? enumerateRuntimeModules(static_cast<pid_t>(binding.ProcessId)) : std::vector<RuntimeProcessModule>{};
                modulePid = binding.ProcessId;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu mapped modules", modules.size());
            ImGui::SetNextItemWidth(410.0f);
            const char* noModuleLabel = binding.AddressMode == ProcessAddressMode::Signature ? "<all mappings>" : "<absolute address>";
            if (ImGui::BeginCombo("Module / base", binding.Module[0] ? binding.Module : noModuleLabel))
            {
                if (ImGui::Selectable(noModuleLabel, binding.Module[0] == '\0')) { binding.Module[0] = '\0'; changed = true; }
                for (const auto& module : modules)
                {
                    std::ostringstream address;
                    address << module.Name << "  0x" << std::hex << module.Base;
                    if (ImGui::Selectable(address.str().c_str(), std::string_view(binding.Module) == module.Name))
                    {
                        std::snprintf(binding.Module, sizeof(binding.Module), "%s", module.Name.c_str());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            static constexpr const char* AddressModes[] = {"Address / pointer chain", "IDA signature"};
            int addressMode = static_cast<int>(binding.AddressMode);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Address source", &addressMode, AddressModes, static_cast<int>(std::size(AddressModes))))
            {
                binding.AddressMode = static_cast<ProcessAddressMode>(addressMode);
                resetRuntimeSignatureScan(binding);
                binding.SignatureConfigHash = 0;
                changed = true;
            }
            if (binding.AddressMode == ProcessAddressMode::AddressChain)
            {
                changed |= ImGui::InputText("Address / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Examples: +0x1234   |   +0x1234 -> +0x28 -> +0x18   |   libgame.so+0x1234 -> +0x20");
            }
            else
            {
                const bool signatureChanged = ImGui::InputText("IDA signature", binding.Signature, sizeof(binding.Signature));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("IDA-style bytes: DE AD ? BE EF ?   |   48 8B 05 ?? ?? ?? ??. Nibble wildcards such as A? and ?F are accepted.");
                changed |= signatureChanged;
                changed |= ImGui::Checkbox("Executable mappings only", &binding.SignatureExecutableOnly);
                static constexpr const char* ResolveModes[] = {"Match address + offset", "x86-64 RIP-relative disp32", "Pointer stored at match + offset", "x86-64 register-relative capture"};
                int resolveMode = static_cast<int>(binding.SignatureResolve);
                ImGui::SetNextItemWidth(300.0f);
                if (ImGui::Combo("Signature result", &resolveMode, ResolveModes, static_cast<int>(std::size(ResolveModes)))) { binding.SignatureResolve = static_cast<SignatureResultMode>(resolveMode); changed = true; }
                ImGui::SetNextItemWidth(180.0f);
                const char* resultOffsetLabel = binding.SignatureResolve == SignatureResultMode::RipRelative32 ? "Displacement offset" : binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture ? "Instruction offset" : "Result offset";
                changed |= ImGui::InputInt(resultOffsetLabel, &binding.SignatureResultOffset);
                if (binding.SignatureResolve == SignatureResultMode::RipRelative32)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    changed |= ImGui::InputInt("Instruction size", &binding.SignatureInstructionSize);
                    binding.SignatureInstructionSize = std::max(binding.SignatureInstructionSize, 1);
                }
                else if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    static constexpr const char* Registers[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
                    int reg = static_cast<int>(binding.SignatureRegister);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::Combo("Base register", &reg, Registers, static_cast<int>(std::size(Registers)))) { binding.SignatureRegister = static_cast<RuntimeX64Register>(reg); changed = true; }
                    static constexpr const char* Displacements[] = {"signed disp8", "signed disp32", "manual"};
                    int displacementType = static_cast<int>(binding.SignatureDisplacementType);
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::Combo("Displacement", &displacementType, Displacements, static_cast<int>(std::size(Displacements)))) { binding.SignatureDisplacementType = static_cast<RuntimeDisplacementType>(displacementType); changed = true; }
                    if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(160.0f);
                        changed |= ImGui::InputInt("Manual displacement", &binding.SignatureManualDisplacement);
                    }
                    else
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(160.0f);
                        changed |= ImGui::InputInt("Displacement offset", &binding.SignatureRegisterDisplacementOffset);
                    }
                    ImGui::SetNextItemWidth(180.0f);
                    changed |= ImGui::DragFloat("Capture timeout", &binding.SignatureCaptureTimeoutSeconds, 0.1f, 0.1f, 120.0f, "%.1f s");
                    ImGui::TextDisabled("A temporary hardware execution breakpoint captures the selected register when the matched instruction executes. The breakpoint is removed immediately after capture; normal reads then use process_vm_readv().");
                }
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::DragFloat("Retry interval", &binding.SignatureRetrySeconds, 0.05f, 0.1f, 60.0f, "%.2f s");
                changed |= ImGui::InputText("Result / pointer chain", binding.Address, sizeof(binding.Address));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The resolved signature address becomes the base. Use +0x0 for the result itself or chains such as +0x0 -> +0x18.");
                if (ImGui::Button("Rescan signature"))
                {
                    resetRuntimeSignatureScan(binding);
                    binding.SignatureConfigHash = 0;
                    binding.NextUpdate = 0.0;
                }
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture && binding.SignatureInstructionAddress != 0 && binding.SignatureResolvedAddress == 0)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Retry register capture"))
                    {
                        binding.SignatureRegisterCapture.reset();
                        binding.NextRegisterCapture = 0.0;
                        binding.NextUpdate = 0.0;
                    }
                }
                ImGui::SameLine();
                if (binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("resolved: 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress));
                else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureResolvedAddress == 0 && binding.SignatureProgress > 0.0f && binding.SignatureInstructionAddress == 0) ImGui::ProgressBar(binding.SignatureProgress, ImVec2(320.0f, 0.0f));
                if (binding.SignatureResolve == SignatureResultMode::RegisterRelativeCapture)
                {
                    if (binding.SignatureMatchAddress != 0) ImGui::TextDisabled("Match 0x%llX   instruction 0x%llX", static_cast<unsigned long long>(binding.SignatureMatchAddress), static_cast<unsigned long long>(binding.SignatureInstructionAddress));
                    if (binding.SignatureCapturedRegister != 0 || binding.SignatureResolvedAddress != 0) ImGui::TextDisabled("%s 0x%llX   displacement %lld", runtimeX64RegisterName(binding.SignatureRegister), static_cast<unsigned long long>(binding.SignatureCapturedRegister), static_cast<long long>(binding.SignatureCapturedDisplacement));
                }
                else ImGui::TextDisabled("The signature locates a stable instruction or data reference. RIP-relative mode resolves common x86-64 global references directly from the matched instruction.");
            }
            static constexpr const char* Types[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "bool"};
            int valueType = static_cast<int>(binding.ValueType);
            if (ImGui::Combo("Value type", &valueType, Types, static_cast<int>(std::size(Types)))) { binding.ValueType = static_cast<ProcessValueType>(valueType); changed = true; }
            ImGui::TextDisabled("Read-only through process_vm_readv(); ptrace_scope/permissions still apply.");
        }
        else if (binding.Source == RuntimeSourceKind::BindingStatus)
        {
            ImGui::SeparatorText("Binding status");
            const RuntimeBinding* target = engine.findBinding(binding.StatusBindingId);
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::BeginCombo("Binding to inspect", target ? target->Name : "<select binding>"))
            {
                for (const auto& candidate : engine.bindings())
                {
                    if (candidate.Id == binding.Id) continue;
                    const bool selected = candidate.Id == binding.StatusBindingId;
                    if (ImGui::Selectable(candidate.Name, selected)) { binding.StatusBindingId = candidate.Id; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Status values are normal 0/1 binding sources, so they can drive shader uniforms, parameter links, or control rules.");
        }

        ImGui::SeparatorText("Material binding");
        changed |= ImGui::Checkbox("Write value to shader material", &binding.WriteMaterial);
        if (binding.WriteMaterial) changed |= drawRuntimeTargetSelector(binding, shaderFramebuffer);
        else ImGui::TextDisabled("Value-only binding: still available to parameter links, status sources and control rules.");

        ImGui::SeparatorText("Transform");
        ImGui::TextDisabled("Transform controls can use local values or the output of another binding. Circular dependencies are rejected when a link is selected and sanitized when bindings are loaded.");
        changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Normalize, "Normalize input", binding.Normalize);
        if (binding.Normalize || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Normalize)].Enabled)
        {
            changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMin, "Input min", binding.InputMin);
            changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::InputMax, "Input max", binding.InputMax);
        }
        changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Invert, "Invert", binding.Invert);
        changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Scale, "Scale", binding.Scale);
        changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::Offset, "Offset", binding.Offset);
        changed |= drawRuntimeLinkedBool(engine, binding, RuntimeParameterSlot::Clamp, "Clamp output", binding.Clamp);
        if (binding.Clamp || binding.ParameterLinks[static_cast<std::size_t>(RuntimeParameterSlot::Clamp)].Enabled)
        {
            changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMin, "Output min", binding.OutputMin);
            changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::OutputMax, "Output max", binding.OutputMax);
        }
        changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::SmoothingHz, "Smoothing", binding.SmoothingHz, 0.1f, "%.1f Hz");
        changed |= drawRuntimeLinkedFloat(engine, binding, RuntimeParameterSlot::UpdateHz, "Update rate", binding.UpdateHz, 0.5f, "%.1f Hz");

        if (binding.HasValue) ImGui::Text("Raw %.6g   ->   value %.6g", binding.RawValue, binding.Value);
        else ImGui::TextDisabled("Waiting for first value...");
        ImGui::SameLine();
        ImGui::TextDisabled("read %s", binding.LastReadSucceeded ? "ok" : "not ready / failed");
        if (!binding.Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "%s", binding.Error.c_str());

        if (changed) engine.markChanged();
        ImGui::PopID();
        ImGui::PopID();
        return changed;
    }

    static const char* runtimeControlConditionName(const RuntimeControlCondition condition)
    {
        static constexpr const char* Names[] = {"==", "!=", "<", "<=", ">", ">=", "between", "outside", "rising edge", "falling edge"};
        return Names[std::clamp(static_cast<int>(condition), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static const char* runtimeControlTargetName(const RuntimeControlTarget target)
    {
        static constexpr const char* Names[] = {"Active shader", "Binding enabled", "Global brightness", "Send framebuffer", "Base color mode", "Material parameter"};
        return Names[std::clamp(static_cast<int>(target), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    static bool drawRuntimeControlMaterialTarget(RuntimeControlRule& control, ShaderFramebuffer& shaderFramebuffer)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Uniform id##control", control.TargetId[0] ? control.TargetId : "<select material uniform>"))
        {
            for (const auto& parameter : shaderFramebuffer.materialParameters())
            {
                const bool selected = parameter.PersistenceKey == control.TargetId;
                const std::string label = parameter.Label + "  [" + parameter.PersistenceKey + "]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    std::snprintf(control.TargetId, sizeof(control.TargetId), "%s", parameter.PersistenceKey.c_str());
                    control.TargetComponent = std::clamp(control.TargetComponent, 0, std::max(parameter.Components - 1, 0));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (const auto* parameter = shaderFramebuffer.findMaterialParameter(control.TargetId); parameter && parameter->Components > 1)
        {
            ImGui::SameLine();
            changed |= ImGui::SliderInt("Component##control", &control.TargetComponent, 0, parameter->Components - 1);
        }
        return changed;
    }

    static bool drawRuntimeControlRule(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer, RuntimeControlRule& control, bool& erase)
    {
        bool changed = false;
        ImGui::PushID("RuntimeControl");
        ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
        const std::string header = std::string(control.Name[0] ? control.Name : "Control") + "###RuntimeControl" + std::to_string(control.Id);
        if (!ImGui::CollapsingHeader(header.c_str())) { ImGui::PopID(); ImGui::PopID(); return false; }
        changed |= ImGui::Checkbox("Enabled", &control.Enabled);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) erase = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        changed |= ImGui::InputText("Name", control.Name, sizeof(control.Name));

        const RuntimeBinding* source = engine.findBinding(control.SourceBindingId);
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::BeginCombo("Source binding", source ? source->Name : "<select binding>"))
        {
            for (const auto& candidate : engine.bindings())
            {
                const bool selected = candidate.Id == control.SourceBindingId;
                if (ImGui::Selectable(candidate.Name, selected)) { control.SourceBindingId = candidate.Id; changed = true; }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        int condition = static_cast<int>(control.Condition);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0rising edge\0falling edge\0")) { control.Condition = static_cast<RuntimeControlCondition>(condition); changed = true; }
        const bool twoValues = control.Condition == RuntimeControlCondition::Between || control.Condition == RuntimeControlCondition::Outside;
        ImGui::SetNextItemWidth(150.0f);
        changed |= ImGui::DragFloat(twoValues ? "Minimum" : "Compare value", &control.ValueA, 0.01f);
        if (twoValues)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            changed |= ImGui::DragFloat("Maximum", &control.ValueB, 0.01f);
        }
        if (control.Condition == RuntimeControlCondition::Equal || control.Condition == RuntimeControlCondition::NotEqual)
        {
            ImGui::SetNextItemWidth(150.0f);
            changed |= ImGui::DragFloat("Tolerance", &control.Tolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f");
        }
        if (control.Condition != RuntimeControlCondition::RisingEdge && control.Condition != RuntimeControlCondition::FallingEdge)
        {
            ImGui::SetNextItemWidth(150.0f);
            changed |= ImGui::DragFloat("Hysteresis", &control.Hysteresis, 0.001f, 0.0f, 1000.0f, "%.4f");
        }

        int target = static_cast<int>(control.Target);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Target", &target, "Active shader\0Binding enabled\0Global brightness\0Send framebuffer\0Base color mode\0Material parameter\0")) { control.Target = static_cast<RuntimeControlTarget>(target); changed = true; }
        if (control.Target == RuntimeControlTarget::ActiveShader)
        {
            const int safeIndex = std::clamp(control.ShaderPresetIndex, 1, static_cast<int>(ShaderPresets.size()));
            const char* preview = ShaderPresets[static_cast<std::size_t>(safeIndex - 1)].Name.c_str();
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("Shader", preview))
            {
                for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
                {
                    const bool selected = control.ShaderPresetIndex == static_cast<int>(i + 1);
                    if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected)) { control.ShaderPresetIndex = static_cast<int>(i + 1); changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Crossfade", &control.TransitionSeconds, 0.02f, 0.0f, 10.0f, "%.2f s");
        }
        else if (control.Target == RuntimeControlTarget::BindingEnabled)
        {
            const RuntimeBinding* targetBinding = engine.findBinding(control.TargetBindingId);
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("Target binding", targetBinding ? targetBinding->Name : "<select binding>"))
            {
                for (const auto& candidate : engine.bindings())
                {
                    if (candidate.Id == control.SourceBindingId) continue;
                    const bool selected = candidate.Id == control.TargetBindingId;
                    if (ImGui::Selectable(candidate.Name, selected)) { control.TargetBindingId = candidate.Id; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            changed |= ImGui::Checkbox("Runtime enabled", &control.TargetBool);
        }
        else if (control.Target == RuntimeControlTarget::GlobalBrightness)
        {
            changed |= ImGui::SliderFloat("Brightness", &control.TargetValue, 0.0f, 1.0f, "%.3f");
        }
        else if (control.Target == RuntimeControlTarget::SendFramebuffer)
        {
            changed |= ImGui::Checkbox("Send framebuffer", &control.TargetBool);
        }
        else if (control.Target == RuntimeControlTarget::BaseColorMode)
        {
            int mode = std::clamp(static_cast<int>(std::lround(control.TargetValue)), 0, 2);
            if (ImGui::Combo("Base color mode", &mode, "RGB wave\0Solid\0Shader\0")) { control.TargetValue = static_cast<float>(mode); changed = true; }
        }
        else if (control.Target == RuntimeControlTarget::MaterialParameter)
        {
            changed |= drawRuntimeControlMaterialTarget(control, shaderFramebuffer);
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat("Target value", &control.TargetValue, 0.01f);
        }

        if (source)
        {
            ImGui::TextDisabled("Source %.6g   condition %s", source->Value, control.ConditionActive ? "active" : "inactive");
        }
        ImGui::TextDisabled("Rules are evaluated in list order; later active rules win when they target the same setting.");
        if (changed) engine.markChanged();
        ImGui::PopID();
        ImGui::PopID();
        return changed;
    }

    static void drawRuntimeBindingsPage(RuntimeBindingEngine& engine, ShaderFramebuffer& shaderFramebuffer)
    {
        ImGui::SeparatorText("Runtime data -> shader material");
        ImGui::TextWrapped("Bindings produce reusable runtime values. They can drive persistent shader material ids, feed other bindings, expose native-process health/status, and act as sources for control rules.");
        if (ImGui::Button("Add binding"))
        {
            engine.add();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save bindings")) engine.save();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", engine.path().string().c_str());

        if (ImGui::CollapsingHeader("Quick bindings"))
        {
            auto addPreset = [&](const char* name, const RuntimeSourceKind source, const int signal, const char* target)
            {
                auto& binding = engine.add();
                std::snprintf(binding.Name, sizeof(binding.Name), "%s", name);
                binding.Source = source;
                binding.Signal = signal;
                std::snprintf(binding.TargetId, sizeof(binding.TargetId), "%s", target);
                binding.Clamp = true;
                binding.OutputMin = 0.0f;
                binding.OutputMax = 1.0f;
                binding.SmoothingHz = 8.0f;
                binding.UpdateHz = source == RuntimeSourceKind::NativeProcess ? 20.0f : 60.0f;
                return &binding;
            };
            if (ImGui::SmallButton("Native -> runtime.native")) addPreset("Native process value", RuntimeSourceKind::NativeProcess, 0, "runtime.native");
            ImGui::SameLine();
            if (ImGui::SmallButton("Status -> runtime.status")) addPreset("Binding status", RuntimeSourceKind::BindingStatus, 0, "runtime.status");
            ImGui::SameLine();
            if (ImGui::SmallButton("Audio -> runtime.audio")) addPreset("Audio RMS", RuntimeSourceKind::Audio, 0, "runtime.audio");
            ImGui::SameLine();
            if (ImGui::SmallButton("CPU -> runtime.system"))
            {
                auto* binding = addPreset("Host CPU", RuntimeSourceKind::Host, 0, "runtime.system");
                binding->Normalize = true;
                binding->InputMin = 0.0f;
                binding->InputMax = 100.0f;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Health tri-color setup"))
            {
                auto addHealthScalar = [&](const char* name, const char* target, const float value, const int component = 0)
                {
                    auto* binding = addPreset(name, RuntimeSourceKind::Constant, 0, target);
                    binding->Constant = value;
                    binding->TargetComponent = component;
                    binding->Normalize = false;
                    binding->Clamp = false;
                    binding->SmoothingHz = 0.0f;
                    return binding;
                };
                addHealthScalar("Health value", "health.value", 100.0f);
                addHealthScalar("Health maximum", "health.maximum", 100.0f);
                addHealthScalar("Healthy R", "health.color.high", 0.05f, 0);
                addHealthScalar("Healthy G", "health.color.high", 1.00f, 1);
                addHealthScalar("Healthy B", "health.color.high", 0.12f, 2);
                addHealthScalar("Half health R", "health.color.mid", 1.00f, 0);
                addHealthScalar("Half health G", "health.color.mid", 0.72f, 1);
                addHealthScalar("Half health B", "health.color.mid", 0.02f, 2);
                addHealthScalar("Critical R", "health.color.low", 1.00f, 0);
                addHealthScalar("Critical G", "health.color.low", 0.02f, 1);
                addHealthScalar("Critical B", "health.color.low", 0.01f, 2);
            }
            ImGui::TextDisabled("Dashboard and data shaders consume persistent material IDs. The health setup creates current/max plus green/yellow/red color bindings that can be replaced with any runtime source.");
        }
        if (shaderFramebuffer.materialParameters().empty()) ImGui::TextDisabled("Current shader has no reflected material parameters. Select a parameterized preset or add an active @ui uniform.");

        std::optional<std::size_t> erase;
        for (std::size_t i = 0; i < engine.bindings().size(); ++i)
        {
            bool shouldErase = false;
            drawRuntimeBinding(engine, shaderFramebuffer, engine.bindings()[i], i, shouldErase);
            if (shouldErase) erase = i;
        }
        if (erase) engine.erase(*erase);
        if (engine.bindings().empty())
        {
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::TextDisabled("No bindings yet. Add one and point it at any reflected shader material id.");
        }

        ImGui::SeparatorText("Control rules");
        ImGui::TextWrapped("Control rules consume any binding value and drive app state. Use normal comparisons/ranges with hysteresis, or edge triggers for one-shot switches. Binding-status sources make process/read/address health usable here too.");
        if (ImGui::Button("Add control rule")) engine.addControl();
        ImGui::SameLine();
        ImGui::TextDisabled("Shader switches can crossfade by rendering the outgoing and incoming shaders simultaneously.");
        std::optional<std::size_t> eraseControl;
        for (std::size_t i = 0; i < engine.controls().size(); ++i)
        {
            bool shouldErase = false;
            drawRuntimeControlRule(engine, shaderFramebuffer, engine.controls()[i], shouldErase);
            if (shouldErase) eraseControl = i;
        }
        if (eraseControl) engine.eraseControl(*eraseControl);
        if (engine.controls().empty()) ImGui::TextDisabled("No control rules yet.");
    }

    static void drawQRPCInspectorPage(RuntimeTelemetry& telemetry)
    {
        ImGui::SeparatorText("QRPC packet inspector");
        static int selectedNewest = 0;
        if (ImGui::Button("Clear packets")) { telemetry.clearPackets(); selectedNewest = 0; }
        ImGui::SameLine();
        ImGui::TextDisabled("Assembled QRPC packets, not raw libusb transfer boundaries. Click a packet for a captured hex view.");
        const auto packets = telemetry.packets();
        if (!packets.empty()) selectedNewest = std::clamp(selectedNewest, 0, static_cast<int>(packets.size()) - 1);
        if (ImGui::BeginTable("QRPCPackets", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 330.0f)))
        {
            ImGui::TableSetupColumn("t");
            ImGui::TableSetupColumn("Dir");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Packet id");
            ImGui::TableSetupColumn("Response for");
            ImGui::TableSetupColumn("Payload");
            ImGui::TableSetupColumn("Packet");
            ImGui::TableSetupColumn("Version");
            ImGui::TableHeadersRow();
            int newestIndex = 0;
            for (auto it = packets.rbegin(); it != packets.rend(); ++it, ++newestIndex)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(newestIndex);
                const bool selected = selectedNewest == newestIndex;
                if (ImGui::Selectable("##packet", selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, ImGui::GetTextLineHeight())))
                    selectedNewest = newestIndex;
                ImGui::SameLine(); ImGui::Text("%.3f", it->Time);
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Tx ? "Host -> Device" : "Device -> Host");
                ImGui::TableNextColumn(); ImGui::Text("%s (0x%04X)", packetTypeLabel(it->Type), it->Type);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->PacketId);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->ResponseFor);
                ImGui::TableNextColumn(); ImGui::Text("%u B", it->PayloadLength);
                ImGui::TableNextColumn(); ImGui::Text("%zu B", it->Bytes);
                ImGui::TableNextColumn(); ImGui::Text("%u", it->Version);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Buffered packets: %zu / 512", packets.size());
        if (!packets.empty())
        {
            const auto& packet = packets[packets.size() - 1 - static_cast<std::size_t>(selectedNewest)];
            ImGui::SeparatorText("Packet bytes");
            ImGui::Text("%s %s   packet %u   %zu/%zu B captured", packet.Tx ? "TX" : "RX", packetTypeLabel(packet.Type), packet.PacketId, packet.CapturedBytes, packet.Bytes);
            if (ImGui::BeginChild("##PacketHex", ImVec2(0.0f, 160.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
            {
                for (std::size_t offset = 0; offset < packet.CapturedBytes; offset += 16)
                {
                    char line[128]{};
                    int written = std::snprintf(line, sizeof(line), "%04zX  ", offset);
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        if (offset + i < packet.CapturedBytes)
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "%02X ", std::to_integer<unsigned>(packet.Data[offset + i]));
                        else
                            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), "   ");
                    }
                    written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written), " |");
                    for (std::size_t i = 0; i < 16 && offset + i < packet.CapturedBytes; ++i)
                    {
                        const unsigned byte = std::to_integer<unsigned>(packet.Data[offset + i]);
                        if (written + 2 < static_cast<int>(sizeof(line))) line[written++] = byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
                    }
                    if (written + 2 < static_cast<int>(sizeof(line))) { line[written++] = '|'; line[written] = '\0'; }
                    ImGui::TextUnformatted(line);
                }
                ImGui::EndChild();
            }
        }
    }

    static void drawUSBProfilerPage(RawUSB& usb, const RuntimeBindingEngine& bindings)
    {
        const auto stats = usb.stats();
        const auto& rates = bindings.usbRates();
        ImGui::SeparatorText("USB transport");
        ImGui::Text("Device: %04X:%04X   interface %d   OUT 0x%02X   IN 0x%02X", VendorId, ProductId, RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("State: %s   product: %s", usb.isConnected() ? "connected" : "disconnected", usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str());
        ImGui::Text("TX %.2f KiB/s  %.1f transfers/s   RX %.2f KiB/s  %.1f transfers/s", rates.TxKiB, rates.TxTransfers, rates.RxKiB, rates.RxTransfers);
        ImGui::Text("TX total %.2f MiB / %llu transfers   RX total %.2f MiB / %llu transfers", stats.TxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.TxTransfers), stats.RxBytes / (1024.0 * 1024.0), static_cast<unsigned long long>(stats.RxTransfers));
        ImGui::Text("Transfer latency: TX %.3f ms   RX %.3f ms", stats.LastTxMilliseconds, stats.LastRxMilliseconds);
        ImGui::Text("Errors: TX %llu   RX %llu   connects %llu   disconnects %llu", static_cast<unsigned long long>(stats.TxErrors), static_cast<unsigned long long>(stats.RxErrors), static_cast<unsigned long long>(stats.Connects), static_cast<unsigned long long>(stats.Disconnects));
        if (usb.lastError() != LIBUSB_SUCCESS) ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, 1.0f), "Last libusb status: %s", libusb_error_name(usb.lastError()));
    }

    static void drawInputAnalyzerPage(const EvdevKeyboard& keyboard, const ReactiveKeyState& keys, const RuntimeInputAnalytics& analytics)
    {
        const float held = std::accumulate(keys.Down.begin(), keys.Down.end(), 0.0f);
        ImGui::SeparatorText("Global keyboard input");
        ImGui::Text("evdev: %s", keyboard.connected() ? "connected" : "disconnected");
        ImGui::TextWrapped("%s", keyboard.status().c_str());
        ImGui::Text("Held mapped keys: %.0f   total presses: %llu   longest press: %.3f s   Caps %s   Scroll %s", held, static_cast<unsigned long long>(analytics.TotalPresses), analytics.LongestPress, keys.CapsLockActive ? "ON" : "off", keys.ScrollLockActive ? "ON" : "off");

        std::array<Color32, MatrixSize> stateFramebuffer{};
        for (std::size_t i = 0; i < MatrixSize; ++i)
            if (keys.Down[i] > 0.5f) stateFramebuffer[i] = {220, 220, 220};
        ImGui::TextUnformatted("Live state");
        drawFramebufferPreview(stateFramebuffer, 0.55f, 532.0f, 0.12f);

        const auto maxPressIt = std::max_element(analytics.PressCount.begin(), analytics.PressCount.end());
        const std::uint64_t maxPresses = maxPressIt != analytics.PressCount.end() ? *maxPressIt : 0;
        std::array<Color32, MatrixSize> heatmap{};
        if (maxPresses != 0)
        {
            for (std::size_t i = 0; i < MatrixSize; ++i)
            {
                const float amount = std::sqrt(static_cast<float>(analytics.PressCount[i]) / static_cast<float>(maxPresses));
                const auto value = static_cast<std::uint8_t>(std::lround(amount * 255.0f));
                heatmap[i] = {value, value, value};
            }
        }
        ImGui::Text("Press-count heatmap   max/key %llu", static_cast<unsigned long long>(maxPresses));
        drawFramebufferPreview(heatmap, 0.55f, 532.0f, 0.08f);

        ImGui::SeparatorText("Recent mapped key events");
        if (ImGui::BeginTable("RecentKeyEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 230.0f)))
        {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Row");
            ImGui::TableSetupColumn("Column");
            ImGui::TableSetupColumn("Age");
            ImGui::TableSetupColumn("Presses");
            ImGui::TableSetupColumn("Held / last");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < keys.Events.size(); ++i)
            {
                const auto& event = keys.Events[i];
                if (event.Valid < 0.5f) continue;
                const std::size_t row = static_cast<std::size_t>(std::clamp(event.Row, 0.0f, static_cast<float>(Rows - 1)));
                const std::size_t column = static_cast<std::size_t>(std::clamp(event.Column, 0.0f, static_cast<float>(Columns - 1)));
                const std::size_t index = row * Columns + column;
                const double heldFor = analytics.heldDuration(index, ImGui::GetTime());
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::Text("%zu", row);
                ImGui::TableNextColumn(); ImGui::Text("%zu", column);
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", std::max(static_cast<float>(ImGui::GetTime()) - event.Time, 0.0f));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(analytics.PressCount[index]));
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", heldFor > 0.0 ? heldFor : analytics.LastDuration[index]);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Ctrl+Alt+Shift+Q remains the global recovery shortcut when the Quartz window is hidden.");
    }

    static void drawRGBProfilerPage(const std::array<Color32, MatrixSize>& framebuffer, const VisualizerSettings& settings, const RuntimeRGBAnalytics& analytics)
    {
        std::array<float, ActiveProbeRows> rowLuma{};
        float r = 0.0f, g = 0.0f, b = 0.0f, lit = 0.0f, peak = 0.0f;
        for (std::size_t row = 0; row < ActiveProbeRows; ++row)
        {
            for (std::size_t column = 0; column < Columns; ++column)
            {
                const auto& color = framebuffer[row * Columns + column];
                const float rf = color.R / 255.0f, gf = color.G / 255.0f, bf = color.B / 255.0f;
                const float luma = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;
                r += rf; g += gf; b += bf; rowLuma[row] += luma; peak = std::max(peak, std::max({rf, gf, bf}));
                if (color.R || color.G || color.B) lit += 1.0f;
            }
            rowLuma[row] /= static_cast<float>(Columns);
        }
        constexpr float Count = static_cast<float>(ActiveProbeRows * Columns);
        r /= Count; g /= Count; b /= Count;
        ImGui::SeparatorText("Framebuffer / RGB profiler");
        ImGui::Text("Output %.1f Hz   changed this frame %zu / %.0f   EMA changed %.1f cells/frame", analytics.FrameRate, analytics.ChangedCells, Count, analytics.AverageChangedCells);
        ImGui::Text("Active cells: %.0f / %.0f (%.1f%%)   peak channel %.1f%%", lit, Count, lit / Count * 100.0f, peak * 100.0f);
        ImGui::Text("Average RGB: %.1f%% / %.1f%% / %.1f%%   luma %.1f%%", r * 100.0f, g * 100.0f, b * 100.0f, (r * 0.2126f + g * 0.7152f + b * 0.0722f) * 100.0f);
        ImGui::Text("Frames profiled %llu   total cell changes %llu   Global brightness %.0f%%   preview interpolation %.0f%%", static_cast<unsigned long long>(analytics.Frames), static_cast<unsigned long long>(analytics.ChangedCellsTotal), settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);
        ImGui::PlotHistogram("Luma distribution", analytics.LumaHistogram.data(), static_cast<int>(analytics.LumaHistogram.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Per-row luma", rowLuma.data(), static_cast<int>(rowLuma.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 100.0f));
        drawFramebufferPreview(framebuffer, 0.55f, 532.0f, settings.LiveOutputInterpolation);
    }

    static void drawAudioLabPage(AudioSpectrum& audio, const AudioLevelSnapshot& level, const AutoGainState& autoGain, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands)
    {
        static std::array<float, 300> rmsHistory{};
        static std::array<float, 300> peakHistory{};
        static double nextHistory = 0.0;
        if (ImGui::GetTime() >= nextHistory)
        {
            std::move(rmsHistory.begin() + 1, rmsHistory.end(), rmsHistory.begin());
            std::move(peakHistory.begin() + 1, peakHistory.end(), peakHistory.begin());
            rmsHistory.back() = level.Rms;
            peakHistory.back() = level.Peak;
            nextHistory = ImGui::GetTime() + 1.0 / 30.0;
        }

        ImGui::SeparatorText("Audio level / normalization");
        ImGui::Text("Capture: %s", audio.source().c_str());
        ImGui::Text("RMS %.5f   peak %.5f", level.Rms, level.Peak);
        ImGui::PlotLines("RMS history", rmsHistory.data(), static_cast<int>(rmsHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 100.0f));
        ImGui::PlotLines("Peak history", peakHistory.data(), static_cast<int>(peakHistory.size()), 0, nullptr, 0.0f, std::max(0.25f, *std::max_element(peakHistory.begin(), peakHistory.end())), ImVec2(-1.0f, 70.0f));

        ImGui::Checkbox("Enable automatic overall gain", &settings.AutomaticOverallGain);
        if (settings.AutomaticOverallGain)
        {
            ImGui::SliderFloat("Baseline gain", &settings.AutoGainBaseline, 0.10f, 6.0f, "%.2fx");
            ImGui::SliderFloat("Target long-term RMS", &settings.AutoGainTargetRms, 0.01f, 0.50f, "%.3f");
            ImGui::SliderFloat("Adaptation speed", &settings.AutoGainAdaptation, 0.02f, 2.0f, "%.2f /s", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Minimum correction", &settings.AutoGainMinCorrection, 0.10f, 1.0f, "%.2fx");
            ImGui::SliderFloat("Maximum correction", &settings.AutoGainMaxCorrection, 1.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Silence gate", &settings.AutoGainSilenceGate, 0.0f, 0.05f, "%.4f");
            ImGui::Text("Learned RMS %.5f   correction %.3fx   effective gain %.3fx", autoGain.LongTermRms, autoGain.Correction, autoGain.EffectiveGain);
            ImGui::TextDisabled("The baseline is the normal gain. Long-term media loudness only applies a slow bounded correction around it; silence is ignored.");
        }
        else
        {
            ImGui::SliderFloat("Manual overall gain", &settings.OverallGain, 0.10f, 4.0f, "%.2fx");
            ImGui::Text("Effective gain %.3fx", autoGain.EffectiveGain);
        }

        ImGui::SeparatorText("Spectrum pipeline");
        ImGui::PlotLines("FFT / log analysis", analysisBands.data(), settings.AnalysisBandCount, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 120.0f));
        ImGui::PlotHistogram("Mapped", mappedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
        ImGui::PlotHistogram("Attack / release output", smoothedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
    }

    static void drawTimelinePage(RuntimeTelemetry& telemetry)
    {
        if (ImGui::Button("Clear timeline")) telemetry.clearEvents();
        ImGui::SameLine();
        ImGui::TextDisabled("USB/media/input/runtime events");
        const auto events = telemetry.events();
        if (ImGui::BeginTable("RuntimeTimeline", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 470.0f)))
        {
            ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.3f", it->Time);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Category.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->Text.c_str());
            }
            ImGui::EndTable();
        }
    }

    static void drawFirmwarePage(const PerformanceSnapshot& performance, const bool hasPerformance, const MatrixTimingProbeResult<ActiveProbeRows>& timingProbe, const bool hasTimingProbe)
    {
        ImGui::SeparatorText("Firmware identity");
        ImGui::Text("Quartz K552X firmware %s", FirmwareVersion);
        ImGui::Text("QRPC protocol v%u   VID:PID %04X:%04X", static_cast<unsigned>(ProtocolVersion), VendorId, ProductId);
        ImGui::Text("RPC interface %d   bulk OUT 0x%02X   bulk IN 0x%02X", RPCInterfaceNumber, RPCOutEndpoint, RPCInEndpoint);
        ImGui::Text("Framebuffer %zux%zu logical / %zux%zu active RGB   Color32 %zu B", Columns, Rows, Columns, ActiveProbeRows, sizeof(Color32));
        ImGui::Text("PacketHeader %zu B   framebuffer payload %zu B", sizeof(PacketHeader), sizeof(FramebufferSetPayload<MatrixSize>));
        ImGui::SeparatorText("Live firmware telemetry");
        if (hasPerformance) drawPerformance(performance); else ImGui::TextDisabled("Waiting for PerformanceResponse...");
        ImGui::SeparatorText("Matrix settle probe");
        if (hasTimingProbe) drawTimingProbe(timingProbe); else ImGui::TextDisabled("No matrix timing probe result received yet.");
    }

    template<typename T>
    static bool defaultButton(const char* id, T& value, const T& defaultValue)
    {
        const bool isDefault = value == defaultValue;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed) value = defaultValue;
        return pressed;
    }

    static bool defaultAudioSourceButton(const char* id, VisualizerSettings& settings, const VisualizerSettings& defaults, AudioSpectrum& audio)
    {
        const bool isDefault = std::strcmp(settings.AudioSource, defaults.AudioSource) == 0;
        ImGui::SameLine();
        if (isDefault) ImGui::BeginDisabled();
        char label[96];
        std::snprintf(label, sizeof(label), "Default##%s", id);
        const bool pressed = ImGui::SmallButton(label);
        if (isDefault) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore default");
        if (pressed)
        {
            std::snprintf(settings.AudioSource, sizeof(settings.AudioSource), "%s", defaults.AudioSource);
            audio.start(settings.AudioSource);
        }
        return pressed;
    }

    static void applyDarkTheme()
    {
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        for (auto& color : style.Colors)
        {
            const float gray = color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
            color.x = gray;
            color.y = gray;
            color.z = gray;
        }
    }

    static constexpr float ShaderEditorBaseSize = 14.0f;
    static constexpr std::array<float, 20> ShaderEditorZoomLevels{0.60f, 0.70f, 0.80f, 0.90f, 1.00f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f, 1.60f, 1.70f, 1.80f, 1.90f, 2.00f, 2.10f, 2.20f, 2.30f, 2.40f, 2.50f};
#if IMGUI_VERSION_NUM >= 19200
    static ImFont* ShaderEditorFont = nullptr;
#else
    static std::array<ImFont*, ShaderEditorZoomLevels.size()> ShaderEditorFonts{};
#endif

    static float shaderEditorPixelSize(const float zoom) noexcept { return std::max(8.0f, std::round(ShaderEditorBaseSize * zoom)); }

    static void initializeShaderEditorFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
#if IMGUI_VERSION_NUM >= 19200
        // ImGui 1.92+ can rasterize the scalable embedded font at the requested size on demand.
        // This avoids stretching one cached bitmap when the editor is zoomed.
        ShaderEditorFont = io.Fonts->AddFontDefaultVector();
#else
        // Older ImGui has fixed-size fonts, so bake one font per zoom level at an integer pixel size.
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            ImFontConfig config{};
            config.SizePixels = shaderEditorPixelSize(ShaderEditorZoomLevels[i]);
            config.OversampleH = 2;
            config.OversampleV = 2;
            config.PixelSnapH = true;
            ShaderEditorFonts[i] = io.Fonts->AddFontDefault(&config);
        }
#endif
    }

    static std::size_t shaderEditorZoomIndex(float& zoom) noexcept
    {
        std::size_t best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < ShaderEditorZoomLevels.size(); ++i)
        {
            const float distance = std::abs(ShaderEditorZoomLevels[i] - zoom);
            if (distance < bestDistance) { bestDistance = distance; best = i; }
        }
        zoom = ShaderEditorZoomLevels[best];
        return best;
    }

    static void updateShaderEditorZoomShortcuts(ShaderEditorState& shaderEditor, VisualizerSettings& settings, const bool editorHovered)
    {
        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return;
        const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool zoomInDown = ctrl && (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS);
        const bool zoomOutDown = ctrl && (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS);
        const bool zoomResetDown = ctrl && glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS;
        if (zoomInDown && !shaderEditor.ZoomInWasDown) settings.ShaderEditorZoom = std::min(settings.ShaderEditorZoom + 0.10f, 2.50f);
        if (zoomOutDown && !shaderEditor.ZoomOutWasDown) settings.ShaderEditorZoom = std::max(settings.ShaderEditorZoom - 0.10f, 0.60f);
        if (zoomResetDown && !shaderEditor.ZoomResetWasDown) settings.ShaderEditorZoom = 1.0f;
        shaderEditor.ZoomInWasDown = zoomInDown;
        shaderEditor.ZoomOutWasDown = zoomOutDown;
        shaderEditor.ZoomResetWasDown = zoomResetDown;
        if (editorHovered && ctrl && ImGui::GetIO().MouseWheel != 0.0f) settings.ShaderEditorZoom = std::clamp(settings.ShaderEditorZoom + ImGui::GetIO().MouseWheel * 0.10f, 0.60f, 2.50f);
        settings.ShaderEditorZoom = std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f;
    }

    static bool renderShaderTextEditor(TextEditor& editor, const char* id, const ImVec2 size, ShaderEditorState& shaderEditor, VisualizerSettings& settings)
    {
        updateShaderEditorZoomShortcuts(shaderEditor, settings, false);
        const std::size_t zoomIndex = shaderEditorZoomIndex(settings.ShaderEditorZoom);
#if IMGUI_VERSION_NUM >= 19200
        ImGui::PushFont(ShaderEditorFont, shaderEditorPixelSize(settings.ShaderEditorZoom));
#else
        ImGui::PushFont(ShaderEditorFonts[zoomIndex] ? ShaderEditorFonts[zoomIndex] : ImGui::GetFont());
#endif
        const bool changed = editor.Render(id, size);
        ImGui::PopFont();
        const bool hovered = ImGui::IsItemHovered();
        updateShaderEditorZoomShortcuts(shaderEditor, settings, hovered);
        return changed;
    }


    static bool drawShaderMaterialEditor(ShaderFramebuffer& shaderFramebuffer, const float maxHeight = 150.0f)
    {
        auto& parameters = shaderFramebuffer.materialParameters();
        if (parameters.empty())
        {
            ImGui::TextDisabled("No reflected user uniforms. Add an active uniform to GLSL, e.g. uniform float uSpeed;");
            ImGui::TextDisabled("Optional metadata: // @ui min=0 max=4 step=0.01 default=1 label=\"Speed\"  |  use @ui color for vec3/vec4.");
            return false;
        }

        bool changed = false;
        const float height = std::min(maxHeight, 34.0f + static_cast<float>(parameters.size()) * ImGui::GetFrameHeightWithSpacing());
        if (ImGui::BeginChild("##ShaderMaterialParameters", ImVec2(0.0f, height), true))
        {
            for (auto& parameter : parameters)
            {
                ImGui::PushID(parameter.Name.c_str());
                ImGui::SetNextItemWidth(std::min(330.0f, ImGui::GetContentRegionAvail().x * 0.58f));
                bool itemChanged = false;
                const float minValue = parameter.HasMin ? parameter.Min : 0.0f;
                const float maxValue = parameter.HasMax ? parameter.Max : 0.0f;
                if (parameter.Boolean)
                {
                    if (parameter.Components == 1)
                    {
                        bool value = parameter.IntValue[0] != 0;
                        if (ImGui::Checkbox(parameter.Label.c_str(), &value)) { parameter.IntValue[0] = value ? 1 : 0; itemChanged = true; }
                    }
                    else
                    {
                        ImGui::TextUnformatted(parameter.Label.c_str());
                        for (int component = 0; component < parameter.Components; ++component)
                        {
                            if (component != 0) ImGui::SameLine();
                            ImGui::PushID(component);
                            bool value = parameter.IntValue[static_cast<std::size_t>(component)] != 0;
                            const char* names[] = {"X", "Y", "Z", "W"};
                            if (ImGui::Checkbox(names[component], &value)) { parameter.IntValue[static_cast<std::size_t>(component)] = value ? 1 : 0; itemChanged = true; }
                            ImGui::PopID();
                        }
                    }
                }
                else if (parameter.Color)
                {
                    itemChanged = parameter.Components == 3
                        ? ImGui::ColorEdit3(parameter.Label.c_str(), parameter.FloatValue.data(), ImGuiColorEditFlags_Float)
                        : ImGui::ColorEdit4(parameter.Label.c_str(), parameter.FloatValue.data(), ImGuiColorEditFlags_Float);
                }
                else if (parameter.Integer)
                {
                    const float speed = std::max(parameter.Step, 1.0f);
                    switch (parameter.Components)
                    {
                    case 1: itemChanged = ImGui::DragInt(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 2: itemChanged = ImGui::DragInt2(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 3: itemChanged = ImGui::DragInt3(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    case 4: itemChanged = ImGui::DragInt4(parameter.Label.c_str(), parameter.IntValue.data(), speed, parameter.HasMin ? static_cast<int>(std::lround(minValue)) : 0, parameter.HasMax ? static_cast<int>(std::lround(maxValue)) : 0); break;
                    default: break;
                    }
                }
                else
                {
                    switch (parameter.Components)
                    {
                    case 1: itemChanged = ImGui::DragFloat(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 2: itemChanged = ImGui::DragFloat2(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 3: itemChanged = ImGui::DragFloat3(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    case 4: itemChanged = ImGui::DragFloat4(parameter.Label.c_str(), parameter.FloatValue.data(), parameter.Step, minValue, maxValue, "%.3f"); break;
                    default: break;
                    }
                }

                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                {
                    if (parameter.Integer || parameter.Boolean) parameter.IntValue = parameter.IntDefault;
                    else parameter.FloatValue = parameter.FloatDefault;
                    itemChanged = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", shaderUniformTypeName(parameter.Type));
                if (ImGui::IsItemHovered())
                {
                    if (parameter.PersistenceKey == parameter.Name)
                        ImGui::SetTooltip("%s", parameter.Name.c_str());
                    else
                        ImGui::SetTooltip("%s\\nPersistent id: %s", parameter.Name.c_str(), parameter.PersistenceKey.c_str());
                }
                changed |= itemChanged;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (changed) shaderFramebuffer.markMaterialChanged();
        return changed;
    }

    static void drawShaderEditorPage(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
        if (ImGui::Button("< Back"))
            page = ViewPage::Main;
        ImGui::SameLine();
        ImGui::TextUnformatted("Shader IDE");
        ImGui::SameLine();
        if (ImGui::Button("Compile"))
        {
            setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
            setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
            if (compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource))
                saveShaderSources(vertexShaderSource, fragmentShaderSource);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
            setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
            saveShaderSources(vertexShaderSource, fragmentShaderSource);
        }
        TextEditor& activeEditor = shaderEditor.ActiveStage == 0 ? shaderEditor.Fragment : shaderEditor.Vertex;
        ImGui::SameLine();
        ImGui::BeginDisabled(!activeEditor.CanUndo());
        if (ImGui::Button("Undo")) activeEditor.Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!activeEditor.CanRedo());
        if (ImGui::Button("Redo")) activeEditor.Redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Find")) activeEditor.OpenFindReplaceWindow();
        ImGui::SameLine();
        ImGui::Checkbox("Recompile on change", &settings.ShaderRecompileOnChange);
        ImGui::SameLine();
        if (ImGui::SmallButton("-##ShaderZoom")) settings.ShaderEditorZoom = std::max(settings.ShaderEditorZoom - 0.10f, 0.60f);
        ImGui::SameLine();
        if (ImGui::SmallButton("100%##ShaderZoom")) settings.ShaderEditorZoom = 1.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("+##ShaderZoom")) settings.ShaderEditorZoom = std::min(settings.ShaderEditorZoom + 0.10f, 2.50f);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%% / %.0f px", settings.ShaderEditorZoom * 100.0f, shaderEditorPixelSize(settings.ShaderEditorZoom));

        const bool compileOk = shaderFramebuffer.status().starts_with("Shaders compiled") || shaderFramebuffer.status().starts_with("Shader framebuffer regenerated");
        const std::string_view fullStatus = shaderFramebuffer.status();
        const std::size_t statusEnd = fullStatus.find('\n');
        const std::string statusSummary(fullStatus.substr(0, statusEnd));
        ImGui::TextColored(compileOk ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", statusSummary.c_str());
        if (!compileOk && statusEnd != std::string_view::npos && ImGui::TreeNode("Compiler log"))
        {
            ImGui::TextWrapped("%s", shaderFramebuffer.status().c_str());
            ImGui::TreePop();
        }

        const char* presetPreview = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[settings.ShaderPresetIndex - 1].Name.c_str();
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::BeginCombo("Preset", presetPreview))
        {
            if (ImGui::Selectable("Custom / current", settings.ShaderPresetIndex == 0))
                settings.ShaderPresetIndex = 0;
            for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            {
                const bool selected = settings.ShaderPresetIndex == static_cast<int>(i + 1);
                if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected))
                {
                    switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, static_cast<int>(i + 1), glfwGetTime(), settings.ShaderTransitionSeconds);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Key-state uniforms", &settings.ShaderKeyStateUniforms);
        ImGui::SameLine();
        ImGui::TextDisabled("evdev -> uCapsLock %.0f   uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reactive presets also receive uKeyState[112] and uKeyEvents[16]. Key capture comes from Linux evdev and works while Quartz is unfocused.");
        ImGui::Checkbox("Caps Lock fixed-color LED", &settings.ShaderCapsLockColorEnabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f);
        ImGui::ColorEdit3("##ShaderCapsLockColorIDE", settings.ShaderCapsLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("Scroll Lock fixed-color LED", &settings.ShaderScrollLockColorEnabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f);
        ImGui::ColorEdit3("##ShaderScrollColorIDE", settings.ShaderScrollLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+wheel/+/- zoom  |  Ctrl+0 reset  |  Ctrl+F find  |  Ctrl+Z/Y undo/redo");
        if (ImGui::CollapsingHeader("Material parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drawShaderMaterialEditor(shaderFramebuffer, 120.0f);
            ImGui::TextDisabled("OpenGL reflects active uniform names/types; @ui comments add editor hints such as ranges, defaults, labels and color pickers.");
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("ShaderStages"))
        {
            if (ImGui::BeginTabItem("Fragment shader"))
            {
                shaderEditor.ActiveStage = 0;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 310.0f);
                ImGui::InputText("##FragmentLoadPathIDE", fragmentLoadPath.data(), fragmentLoadPath.size());
                ImGui::SameLine();
                if (ImGui::Button("Load from file##fragment"))
                {
                    if (loadTextFile(fragmentLoadPath.data(), fragmentShaderSource))
                    {
                        settings.ShaderPresetIndex = 0;
                        shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
                        if (settings.ShaderRecompileOnChange)
                            compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Default##fragment"))
                {
                    settings.ShaderPresetIndex = 1;
                    setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
                    shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
                }
                const float previewReserve = 280.0f;
                const float editorHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - previewReserve - ImGui::GetTextLineHeightWithSpacing());
                const bool changed = renderShaderTextEditor(shaderEditor.Fragment, "##FragmentEditor", ImVec2(-1.0f, editorHeight), shaderEditor, settings);
                const auto fragmentCursor = shaderEditor.Fragment.GetCurrentCursorPosition();
                ImGui::TextDisabled("Fragment  |  Ln %zu, Col %zu  |  %zu lines  |  GLSL", fragmentCursor.line + 1, fragmentCursor.index + 1, shaderEditor.Fragment.GetLineCount());
                if (changed)
                {
                    settings.ShaderPresetIndex = 0;
                    setShaderSource(fragmentShaderSource, shaderEditor.Fragment.GetText());
                    saveTextFile(fragmentShaderPath(), fragmentShaderSource);
                    if (settings.ShaderRecompileOnChange)
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                }
                drawShaderLivePanel(usb, deviceState, keyboardInput, framebuffer, appCpuUsage, scrollLockActive, capsLockActive, settings);
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Vertex shader"))
            {
                shaderEditor.ActiveStage = 1;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 310.0f);
                ImGui::InputText("##VertexLoadPathIDE", vertexLoadPath.data(), vertexLoadPath.size());
                ImGui::SameLine();
                if (ImGui::Button("Load from file##vertex"))
                {
                    if (loadTextFile(vertexLoadPath.data(), vertexShaderSource))
                    {
                        shaderEditor.Vertex.SetText(vertexShaderSource.data());
                        saveTextFile(vertexShaderPath(), vertexShaderSource);
                        if (settings.ShaderRecompileOnChange)
                            compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Default##vertex"))
                {
                    setShaderSource(vertexShaderSource, DefaultVertexShaderSource);
                    shaderEditor.Vertex.SetText(vertexShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
                }
                const float previewReserve = 280.0f;
                const float editorHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - previewReserve - ImGui::GetTextLineHeightWithSpacing());
                const bool changed = renderShaderTextEditor(shaderEditor.Vertex, "##VertexEditor", ImVec2(-1.0f, editorHeight), shaderEditor, settings);
                const auto vertexCursor = shaderEditor.Vertex.GetCurrentCursorPosition();
                ImGui::TextDisabled("Vertex  |  Ln %zu, Col %zu  |  %zu lines  |  GLSL", vertexCursor.line + 1, vertexCursor.index + 1, shaderEditor.Vertex.GetLineCount());
                if (changed)
                {
                    setShaderSource(vertexShaderSource, shaderEditor.Vertex.GetText());
                    saveTextFile(vertexShaderPath(), vertexShaderSource);
                    if (settings.ShaderRecompileOnChange)
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                }
                drawShaderLivePanel(usb, deviceState, keyboardInput, framebuffer, appCpuUsage, scrollLockActive, capsLockActive, settings);
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    static void hideWindow(GLFWwindow* window)
    {
        if (window) glfwHideWindow(window);
    }

    static void restoreWindow(GLFWwindow* window)
    {
        if (!window) return;
        glfwShowWindow(window);
        glfwRestoreWindow(window);
        glfwFocusWindow(window);
    }

    static void drawPermanentHeader(RawUSB& usb)
    {
        ImGui::Text("Quartz K552X  |  USB %s  |  FW %s  |  %04X:%04X", usb.isConnected() ? "connected" : "disconnected", FirmwareVersion, VendorId, ProductId);
        ImGui::SameLine();
        if (ImGui::SmallButton("Terminate")) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exit Quartz completely. The normal window close button only hides the window.");
        const char* credits = "Made by Raony Reis, not affiliated with Redragon";
        const float creditWidth = ImGui::CalcTextSize(credits).x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetCursorPosX() + creditWidth + 16.0f < right) ImGui::SameLine(right - creditWidth);
        ImGui::TextDisabled("%s", credits);
        ImGui::Separator();
    }

    static void drawUi(RawUSB& usb, AudioSpectrum& audio, MediaColorProvider& mediaColor, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands, const std::array<Color32, MatrixSize>& framebuffer, SharedDeviceState& deviceState, RuntimeBindingEngine& runtimeBindings, RuntimeTelemetry& runtimeTelemetry, const AutoGainState& autoGain, const AudioLevelSnapshot& audioLevel, const ReactiveKeyState& reactiveKeys, const RuntimeInputAnalytics& inputAnalytics, const RuntimeRGBAnalytics& rgbAnalytics, std::uint64_t sentFrames, std::uint64_t droppedFrames, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Quartz K552X Visualizer", nullptr, windowFlags);
        drawPermanentHeader(usb);
        if (page == ViewPage::ShaderEditor)
        {
            drawShaderEditorPage(usb, deviceState, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, framebuffer, appCpuUsage, scrollLockActive, capsLockActive);
            ImGui::End();
            return;
        }
        static const VisualizerSettings defaults{};
        const bool connected = usb.isConnected();
        ImGui::Text("USB: %s", connected ? "connected" : "disconnected");
        ImGui::SameLine();
        if (!connected && ImGui::Button("Connect"))
            usb.connect();
        if (connected)
        {
            ImGui::SameLine();
            if (ImGui::Button("Disconnect"))
            {
                settings.AutoReconnect = false;
                usb.disconnect();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%04X:%04X", VendorId, ProductId);
        ImGui::SameLine();
        ImGui::Checkbox("Auto reconnect", &settings.AutoReconnect);
        defaultButton("AutoReconnect", settings.AutoReconnect, defaults.AutoReconnect);
        ImGui::SliderFloat("Global brightness", &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        defaultButton("GlobalBrightness", settings.GlobalBrightness, defaults.GlobalBrightness);
        ImGui::SliderFloat("Live output interpolation", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview-only spatial color mixing. 0 = exact framebuffer, 1 = strongest neighboring-key blend. It never changes data sent over USB.");
        defaultButton("LiveOutputInterpolation", settings.LiveOutputInterpolation, defaults.LiveOutputInterpolation);
        ImGui::SameLine();
        if (ImGui::Button("Reset all settings"))
        {
            const bool restartAudio = std::strcmp(settings.AudioSource, defaults.AudioSource) != 0;
            settings = defaults;
            if (restartAudio) audio.start(settings.AudioSource);
        }
        if (!connected && usb.lastError() != LIBUSB_SUCCESS)
            ImGui::TextDisabled("libusb: %s", libusb_error_name(usb.lastError()));
        ImGui::Text("Frames sent: %llu   busy/dropped: %llu", static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames));
        const std::string configPath = settingsPath().string();
        ImGui::TextDisabled("Settings: %s", configPath.c_str());
        ImGui::TextDisabled("%s", g_SettingsStatus.c_str());

        if (ImGui::BeginTabBar("MainTabs"))
        {
            if (ImGui::BeginTabItem("Visualizer"))
            {
                ImGui::Checkbox("Enabled", &settings.Enabled);
                defaultButton("Enabled", settings.Enabled, defaults.Enabled);
                ImGui::Checkbox("Send framebuffer", &settings.SendFramebuffer);
                defaultButton("SendFramebuffer", settings.SendFramebuffer, defaults.SendFramebuffer);
                ImGui::SliderInt("Frame rate", &settings.FrameRate, 30, 500, "%d Hz");
                defaultButton("FrameRate", settings.FrameRate, defaults.FrameRate);
                ImGui::Checkbox("Yield main loop", &settings.LimitMainLoop);
                defaultButton("LimitMainLoop", settings.LimitMainLoop, defaults.LimitMainLoop);
                ImGui::SliderInt("Analysis bands", &settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
                defaultButton("AnalysisBandCount", settings.AnalysisBandCount, defaults.AnalysisBandCount);
                ImGui::Checkbox("Automatic overall gain", &settings.AutomaticOverallGain);
                defaultButton("AutomaticOverallGain", settings.AutomaticOverallGain, defaults.AutomaticOverallGain);
                if (settings.AutomaticOverallGain)
                {
                    ImGui::SliderFloat("Baseline gain", &settings.AutoGainBaseline, 0.10f, 6.00f, "%.2fx");
                    defaultButton("AutoGainBaseline", settings.AutoGainBaseline, defaults.AutoGainBaseline);
                    ImGui::Text("Auto gain: RMS %.4f  learned %.4f  correction %.3fx  effective %.3fx", audioLevel.Rms, autoGain.LongTermRms, autoGain.Correction, autoGain.EffectiveGain);
                    if (ImGui::TreeNode("Automatic gain tuning"))
                    {
                        ImGui::SliderFloat("Target RMS", &settings.AutoGainTargetRms, 0.01f, 0.50f, "%.3f");
                        defaultButton("AutoGainTargetRms", settings.AutoGainTargetRms, defaults.AutoGainTargetRms);
                        ImGui::SliderFloat("Adaptation", &settings.AutoGainAdaptation, 0.02f, 2.0f, "%.2f /s", ImGuiSliderFlags_Logarithmic);
                        defaultButton("AutoGainAdaptation", settings.AutoGainAdaptation, defaults.AutoGainAdaptation);
                        ImGui::SliderFloat("Min correction", &settings.AutoGainMinCorrection, 0.10f, 1.0f, "%.2fx");
                        defaultButton("AutoGainMinCorrection", settings.AutoGainMinCorrection, defaults.AutoGainMinCorrection);
                        ImGui::SliderFloat("Max correction", &settings.AutoGainMaxCorrection, 1.0f, 8.0f, "%.2fx");
                        defaultButton("AutoGainMaxCorrection", settings.AutoGainMaxCorrection, defaults.AutoGainMaxCorrection);
                        ImGui::SliderFloat("Silence gate", &settings.AutoGainSilenceGate, 0.0f, 0.05f, "%.4f");
                        defaultButton("AutoGainSilenceGate", settings.AutoGainSilenceGate, defaults.AutoGainSilenceGate);
                        ImGui::TextDisabled("Baseline is the normal gain; the long-term loudness estimate only applies a slow bounded correction around it.");
                        ImGui::TreePop();
                    }
                }
                else
                {
                    ImGui::SliderFloat("Overall gain", &settings.OverallGain, 0.10f, 4.00f, "%.2fx");
                    defaultButton("OverallGain", settings.OverallGain, defaults.OverallGain);
                }
                ImGui::SliderFloat("Attack", &settings.AttackSpeed, 0.1f, 80.0f, "%.2f");
                defaultButton("AttackSpeed", settings.AttackSpeed, defaults.AttackSpeed);
                ImGui::SliderFloat("Release", &settings.ReleaseSpeed, 0.1f, 80.0f, "%.2f");
                defaultButton("ReleaseSpeed", settings.ReleaseSpeed, defaults.ReleaseSpeed);
                ImGui::SliderFloat("Feather rows", &settings.FeatherRows, 0.10f, 7.0f, "%.2f");
                defaultButton("FeatherRows", settings.FeatherRows, defaults.FeatherRows);
                ImGui::SliderFloat("Saturation", &settings.Saturation, 0.0f, 4.0f, "%.2fx");
                defaultButton("Saturation", settings.Saturation, defaults.Saturation);
                ImGui::SeparatorText("Spectrum mapping");
                ImGui::SliderInt("Bass columns", &settings.BassColumns, 2, 8);
                defaultButton("BassColumns", settings.BassColumns, defaults.BassColumns);
                ImGui::SliderInt("Bass end band", &settings.BassEndBand, 0, std::max(1, settings.AnalysisBandCount - 1));
                defaultButton("BassEndBand", settings.BassEndBand, defaults.BassEndBand);
                ImGui::SliderFloat("Bass activation", &settings.BassActivationThreshold, 0.0f, 0.99f, "%.2f");
                defaultButton("BassActivationThreshold", settings.BassActivationThreshold, defaults.BassActivationThreshold);
                ImGui::SliderFloat("Bass max boost", &settings.BassMaxBoost, 1.0f, 4.0f, "%.2fx");
                defaultButton("BassMaxBoost", settings.BassMaxBoost, defaults.BassMaxBoost);
                if (ImGui::TreeNode("Per-column gain"))
                {
                    for (std::size_t i = 0; i < Columns; ++i)
                    {
                        char label[32];
                        std::snprintf(label, sizeof(label), "Column %zu", i);
                        ImGui::SliderFloat(label, &settings.ColumnGain[i], 0.0f, 2.5f, "%.2f");
                        char resetId[32];
                        std::snprintf(resetId, sizeof(resetId), "ColumnGain%zu", i);
                        defaultButton(resetId, settings.ColumnGain[i], defaults.ColumnGain[i]);
                    }
                    ImGui::TreePop();
                }
                ImGui::SeparatorText("Color");
                const char* modes[] = {"RGB wave", "Solid", "Shader (full framebuffer)"};
                ImGui::Combo("Base color", &settings.BaseColorMode, modes, 3);
                defaultButton("BaseColorMode", settings.BaseColorMode, defaults.BaseColorMode);
                if (settings.BaseColorMode == 0)
                {
                    ImGui::SliderFloat("Wave speed", &settings.WaveSpeed, -2.0f, 2.0f, "%.3f");
                    defaultButton("WaveSpeed", settings.WaveSpeed, defaults.WaveSpeed);
                }
                else if (settings.BaseColorMode == 1)
                {
                    ImGui::ColorEdit3("Solid color", settings.SolidColor.data());
                    defaultButton("SolidColor", settings.SolidColor, defaults.SolidColor);
                }
                else
                {
                    const char* downsampleModes[] = {"Average logical cell (smooth)", "Average center 4x4", "Center pixel (exact)"};
                    int requestedSize[2]{settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight};
                    if (ImGui::InputInt2("Shader framebuffer size", requestedSize))
                    {
                        settings.ShaderFramebufferWidth = std::clamp(requestedSize[0], static_cast<int>(Columns), MaxShaderDimension);
                        settings.ShaderFramebufferHeight = std::clamp(requestedSize[1], static_cast<int>(Rows), MaxShaderDimension);
                    }
                    defaultButton("ShaderFramebufferWidth", settings.ShaderFramebufferWidth, defaults.ShaderFramebufferWidth);
                    defaultButton("ShaderFramebufferHeight", settings.ShaderFramebufferHeight, defaults.ShaderFramebufferHeight);
                    ImGui::SameLine();
                    if (ImGui::Button("Regenerate framebuffer"))
                        shaderFramebuffer.regenerate(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
                    ImGui::Text("Active shader surface: %dx%d -> 16x7 QRPC framebuffer", shaderFramebuffer.width(), shaderFramebuffer.height());
                    ImGui::Combo("Framebuffer approximation", &settings.ShaderDownsampleMode, downsampleModes, 3);
                    defaultButton("ShaderDownsampleMode", settings.ShaderDownsampleMode, defaults.ShaderDownsampleMode);
                    ImGui::SliderFloat("Shader crossfade", &settings.ShaderTransitionSeconds, 0.0f, 5.0f, "%.2f s");
                    defaultButton("ShaderTransitionSeconds", settings.ShaderTransitionSeconds, defaults.ShaderTransitionSeconds);
                    ImGui::SameLine();
                    ImGui::TextDisabled(shaderTransition.Active ? "transition active" : "manual/control shader switch transition");
                    ImGui::Checkbox("Recompile on text change", &settings.ShaderRecompileOnChange);
                    defaultButton("ShaderRecompileOnChange", settings.ShaderRecompileOnChange, defaults.ShaderRecompileOnChange);
                    ImGui::Checkbox("Key-state shader uniforms", &settings.ShaderKeyStateUniforms);
                    defaultButton("ShaderKeyStateUniforms", settings.ShaderKeyStateUniforms, defaults.ShaderKeyStateUniforms);
                    ImGui::SameLine();
                    ImGui::TextDisabled("evdev -> uCapsLock %.0f  uScrollLock %.0f", settings.ShaderKeyStateUniforms && capsLockActive ? 1.0f : 0.0f, settings.ShaderKeyStateUniforms && scrollLockActive ? 1.0f : 0.0f);
                    ImGui::Checkbox("Caps Lock uses fixed shader color", &settings.ShaderCapsLockColorEnabled);
                    defaultButton("ShaderCapsLockColorEnabled", settings.ShaderCapsLockColorEnabled, defaults.ShaderCapsLockColorEnabled);
                    ImGui::ColorEdit3("Caps Lock shader color", settings.ShaderCapsLockColor.data());
                    defaultButton("ShaderCapsLockColor", settings.ShaderCapsLockColor, defaults.ShaderCapsLockColor);
                    ImGui::Checkbox("Scroll Lock uses fixed shader color", &settings.ShaderScrollLockColorEnabled);
                    defaultButton("ShaderScrollLockColorEnabled", settings.ShaderScrollLockColorEnabled, defaults.ShaderScrollLockColorEnabled);
                    ImGui::ColorEdit3("Scroll Lock shader color", settings.ShaderScrollLockColor.data());
                    defaultButton("ShaderScrollLockColor", settings.ShaderScrollLockColor, defaults.ShaderScrollLockColor);

                    const char* presetPreview = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[settings.ShaderPresetIndex - 1].Name.c_str();
                    if (ImGui::BeginCombo("Shader preset", presetPreview))
                    {
                        if (ImGui::Selectable("Custom / current", settings.ShaderPresetIndex == 0))
                            settings.ShaderPresetIndex = 0;
                        for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
                        {
                            const bool selected = settings.ShaderPresetIndex == static_cast<int>(i + 1);
                            if (ImGui::Selectable(ShaderPresets[i].Name.c_str(), selected))
                            {
                                switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, static_cast<int>(i + 1), glfwGetTime(), settings.ShaderTransitionSeconds);
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (defaultButton("ShaderPresetIndex", settings.ShaderPresetIndex, defaults.ShaderPresetIndex))
                    {
                        const int presetIndex = std::clamp(settings.ShaderPresetIndex, 1, static_cast<int>(ShaderPresets.size()));
                        switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, presetIndex, glfwGetTime(), settings.ShaderTransitionSeconds);
                    }
                    ImGui::SeparatorText("Material parameters");
                    drawShaderMaterialEditor(shaderFramebuffer, 170.0f);
                    ImGui::TextDisabled("Arbitrary active float/int/bool/vector uniforms are reflected automatically. Engine uniforms stay reserved unless explicitly annotated with // @ui.");
                    ImGui::TextDisabled("Uniforms: uTime, uResolution, uBands[16], uMediaColor, uMediaAmount, uSolidColor, uWaveSpeed, uFeatherRows, uSaturation, uForceFullRow, uFullRow");
                    if (ImGui::Button("Compile shaders"))
                    {
                        if (compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource))
                            saveShaderSources(vertexShaderSource, fragmentShaderSource);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Default shaders"))
                    {
                        settings.ShaderPresetIndex = 1;
                        setShaderSource(vertexShaderSource, DefaultVertexShaderSource);
                        setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
                        if (shaderEditor.Initialized) { shaderEditor.Vertex.SetText(vertexShaderSource.data()); shaderEditor.Fragment.SetText(fragmentShaderSource.data()); }
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                        saveShaderSources(vertexShaderSource, fragmentShaderSource);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", shaderFramebuffer.status().c_str());

                    if (ImGui::Button("Edit shaders..."))
                    {
                        initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
                        shaderEditor.Vertex.SetText(vertexShaderSource.data());
                        shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                        updateShaderDiagnostics(shaderEditor, shaderFramebuffer.status());
                        page = ViewPage::ShaderEditor;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("GLSL IDE: syntax highlighting, line numbers, minimap and compiler diagnostics");
                }
                ImGui::Checkbox("Use MPRIS artwork color", &settings.MediaArtworkColor);
                defaultButton("MediaArtworkColor", settings.MediaArtworkColor, defaults.MediaArtworkColor);
                ImGui::SliderFloat("Artwork blend", &settings.MediaColorBlend, 0.0f, 1.0f, "%.2f");
                defaultButton("MediaColorBlend", settings.MediaColorBlend, defaults.MediaColorBlend);
                ImGui::SliderFloat("Color transition", &settings.ColorTransitionSpeed, 0.1f, 12.0f, "%.2f");
                defaultButton("ColorTransitionSpeed", settings.ColorTransitionSpeed, defaults.ColorTransitionSpeed);
                ImGui::Checkbox("Force one row full", &settings.ForceFullRow);
                defaultButton("ForceFullRow", settings.ForceFullRow, defaults.ForceFullRow);
                if (settings.ForceFullRow)
                {
                    ImGui::SliderInt("Full row", &settings.FullRow, 0, static_cast<int>(Rows) - 2);
                    defaultButton("FullRow", settings.FullRow, defaults.FullRow);
                }
                const auto mediaTarget = mediaColor.targetColor();
                const auto mediaStatus = mediaColor.status();
                const auto mediaTitle = mediaColor.mediaTitle();
                ImGui::Text("Media: %s", mediaStatus.c_str());
                if (!mediaTitle.empty())
                    ImGui::TextWrapped("%s", mediaTitle.c_str());
                if (mediaTarget)
                {
                    const float color[4] = {mediaTarget->R / 255.0f, mediaTarget->G / 255.0f, mediaTarget->B / 255.0f, 1.0f};
                    ImGui::ColorButton("Artwork color", ImVec4(color[0], color[1], color[2], color[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(42, 22));
                }
                if (!MediaColorProvider::imageDecoderAvailable())
                    ImGui::TextDisabled("Artwork extraction disabled: stb_image.h was not found at compile time.");
                ImGui::SeparatorText("Audio");
                static std::vector<AudioSourceInfo> audioSources = enumerateAudioSources();
                const auto selectedSource = std::ranges::find_if(audioSources, [&](const auto& source) { return source.Name == settings.AudioSource; });
                const char* sourcePreview = selectedSource != audioSources.end() ? selectedSource->Description.c_str() : settings.AudioSource;
                if (ImGui::BeginCombo("Capture device", sourcePreview))
                {
                    for (const auto& source : audioSources)
                    {
                        const bool selected = source.Name == settings.AudioSource;
                        const std::string label = source.Description == source.Name ? source.Name : source.Description + "##" + source.Name;
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            std::snprintf(settings.AudioSource, sizeof(settings.AudioSource), "%s", source.Name.c_str());
                            audio.start(settings.AudioSource);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        if (ImGui::IsItemHovered() && source.Description != source.Name)
                            ImGui::SetTooltip("%s", source.Name.c_str());
                    }
                    ImGui::EndCombo();
                }
                defaultAudioSourceButton("AudioSource", settings, defaults, audio);
                ImGui::SameLine();
                if (ImGui::Button("Refresh devices"))
                    audioSources = enumerateAudioSources();
                if (audioSources.empty())
                    ImGui::TextDisabled("No Pulse/PipeWire sources found (pactl).");
                ImGui::InputText("Source name", settings.AudioSource, sizeof(settings.AudioSource));
                defaultAudioSourceButton("AudioSourceText", settings, defaults, audio);
                ImGui::SameLine();
                if (ImGui::Button("Restart audio"))
                    audio.start(settings.AudioSource);
                ImGui::Text("Capture: %s", audio.isRunning() ? "running" : "stopped");
                if (!audio.error().empty())
                    ImGui::TextDisabled("%s", audio.error().c_str());
                ImGui::SliderFloat("Min frequency", &settings.MinFrequency, 20.0f, 1000.0f, "%.0f Hz");
                defaultButton("MinFrequency", settings.MinFrequency, defaults.MinFrequency);
                ImGui::SliderFloat("Max frequency", &settings.MaxFrequency, 1000.0f, 22000.0f, "%.0f Hz");
                defaultButton("MaxFrequency", settings.MaxFrequency, defaults.MaxFrequency);
                ImGui::SliderFloat("Min dB", &settings.MinDb, -120.0f, -20.0f, "%.1f dB");
                defaultButton("MinDb", settings.MinDb, defaults.MinDb);
                ImGui::SliderFloat("Max dB", &settings.MaxDb, -40.0f, 0.0f, "%.1f dB");
                defaultButton("MaxDb", settings.MaxDb, defaults.MaxDb);
                ImGui::SliderFloat("MPRIS poll", &settings.MediaPollInterval, 0.10f, 3.0f, "%.2f s");
                defaultButton("MediaPollInterval", settings.MediaPollInterval, defaults.MediaPollInterval);
                ImGui::SliderFloat("Stats poll", &settings.StatisticsInterval, 0.05f, 2.0f, "%.2f s");
                defaultButton("StatisticsInterval", settings.StatisticsInterval, defaults.StatisticsInterval);
                if (ImGui::Button("Black out") && connected)
                {
                    std::array<Color32, MatrixSize> black{};
                    sendFramebuffer(usb, black);
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Spectrum"))
            {
                ImGui::Checkbox("Analysis graph", &settings.ShowAnalysisSpectrum);
                defaultButton("ShowAnalysisSpectrum", settings.ShowAnalysisSpectrum, defaults.ShowAnalysisSpectrum);
                ImGui::Checkbox("Mapped graph", &settings.ShowMappedSpectrum);
                defaultButton("ShowMappedSpectrum", settings.ShowMappedSpectrum, defaults.ShowMappedSpectrum);
                ImGui::Checkbox("Framebuffer preview", &settings.ShowFramebuffer);
                defaultButton("ShowFramebuffer", settings.ShowFramebuffer, defaults.ShowFramebuffer);
                if (settings.ShowAnalysisSpectrum)
                {
                    ImGui::TextUnformatted("FFT / log-frequency analysis");
                    ImGui::PlotLines("##analysis", analysisBands.data(), settings.AnalysisBandCount, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 150.0f));
                }
                if (settings.ShowMappedSpectrum)
                {
                    ImGui::TextUnformatted("Mapped 16 columns");
                    ImGui::PlotHistogram("##mapped", mappedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 110.0f));
                    ImGui::TextUnformatted("Smoothed output");
                    ImGui::PlotHistogram("##smoothed", smoothedBands.data(), static_cast<int>(Columns), 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 110.0f));
                }
                if (settings.ShowFramebuffer)
                {
                    ImGui::TextUnformatted("Keyboard framebuffer");
                    drawFramebufferPreview(framebuffer, 0.55f, 532.0f, settings.LiveOutputInterpolation);
                    ImGui::TextDisabled("Preview interpolation %.0f%% (visual only)", settings.LiveOutputInterpolation * 100.0f);
                }
                ImGui::EndTabItem();
            }

            PerformanceSnapshot performance;
            MatrixTimingProbeResult<ActiveProbeRows> timingProbe;
            bool hasPerformance;
            bool hasTimingProbe;
            std::uint64_t receivedPackets;
            {
                std::lock_guard lock(deviceState.Mutex);
                performance = deviceState.Performance;
                timingProbe = deviceState.TimingProbe;
                hasPerformance = deviceState.HasPerformance;
                hasTimingProbe = deviceState.HasTimingProbe;
                receivedPackets = deviceState.ReceivedPackets;
            }

            if (ImGui::BeginTabItem("Device"))
            {
                const std::size_t framebufferPayloadBytes = sizeof(FramebufferSetPayload<MatrixSize>);
                const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + framebufferPayloadBytes;
                const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
                const char* outputMode = settings.BaseColorMode == 0 ? "RGB wave" : settings.BaseColorMode == 1 ? "Solid" : "Shader framebuffer";
                const char* shaderName = settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str();

                ImGui::SeparatorText("Identity");
                ImGui::Text("Product: %s", connected ? (usb.deviceName().empty() ? "Quartz K552X" : usb.deviceName().c_str()) : "Disconnected");
                ImGui::Text("Firmware: %s", FirmwareVersion);
                ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
                ImGui::Text("QRPC protocol: v%u   interface %d   OUT 0x%02X   IN 0x%02X", static_cast<unsigned>(ProtocolVersion), RPCInterfaceNumber, static_cast<unsigned>(RPCOutEndpoint), static_cast<unsigned>(RPCInEndpoint));
                ImGui::Text("Packet header: %zu B   framebuffer payload: %zu B   full frame packet: %zu B", sizeof(PacketHeader), framebufferPayloadBytes, framebufferPacketBytes);
                ImGui::Text("Logical framebuffer: %zux%zu = %zu cells   active RGB area: %zux%zu", Columns, Rows, MatrixSize, Columns, ActiveProbeRows);

                ImGui::SeparatorText("Session / output");
                ImGui::Text("Client uptime: %.1f s   App CPU: %.2f%%", ImGui::GetTime(), appCpuUsage);
                ImGui::Text("TX frames: %llu   dropped/busy: %llu   RX packets: %llu", static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames), static_cast<unsigned long long>(receivedPackets));
                ImGui::Text("Output mode: %s   target: %d Hz   estimated framebuffer TX: %.1f KiB/s", outputMode, settings.FrameRate, configuredTxKiB);
                if (settings.BaseColorMode == 2) ImGui::Text("Shader: %s   render target: %dx%d   downsample mode: %d   material params: %zu", shaderName, settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight, settings.ShaderDownsampleMode, shaderFramebuffer.materialParameters().size());
                ImGui::Text("Brightness: %.0f%%   live-preview interpolation: %.0f%%", settings.GlobalBrightness * 100.0f, settings.LiveOutputInterpolation * 100.0f);

                ImGui::SeparatorText("Host input / window");
                ImGui::Text("evdev: %s", keyboardInput.connected() ? "connected" : "disconnected");
                if (!keyboardInput.deviceName().empty()) ImGui::Text("Input device: %s", keyboardInput.deviceName().c_str());
                ImGui::TextWrapped("%s", keyboardInput.status().c_str());
                ImGui::Text("Caps Lock: %s   Scroll Lock: %s", capsLockActive ? "on" : "off", scrollLockActive ? "on" : "off");
                ImGui::TextDisabled("Closing the GLFW window hides it. Ctrl + Alt + Shift + Q restores it globally. Use Terminate in the permanent header to actually exit.");

                ImGui::SeparatorText("Runtime files");
                const std::string configFile = settingsPath().string();
                const std::string vertexFile = vertexShaderPath().string();
                const std::string fragmentFile = fragmentShaderPath().string();
                const std::string materialFile = shaderMaterialPath().string();
                const std::string bindingsFile = runtimeBindings.path().string();
                ImGui::TextWrapped("Settings: %s", configFile.c_str());
                ImGui::TextWrapped("Vertex shader: %s", vertexFile.c_str());
                ImGui::TextWrapped("Fragment shader: %s", fragmentFile.c_str());
                ImGui::TextWrapped("Material parameters: %s", materialFile.c_str());
                ImGui::TextWrapped("Runtime bindings: %s", bindingsFile.c_str());
                ImGui::EndTabItem();
            }


            if (ImGui::BeginTabItem("RE / Bindings"))
            {
                drawRuntimeBindingsPage(runtimeBindings, shaderFramebuffer);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("QRPC Inspector"))
            {
                drawQRPCInspectorPage(runtimeTelemetry);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("USB"))
            {
                drawUSBProfilerPage(usb, runtimeBindings);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Input Analyzer"))
            {
                drawInputAnalyzerPage(keyboardInput, reactiveKeys, inputAnalytics);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("RGB Profiler"))
            {
                drawRGBProfilerPage(framebuffer, settings, rgbAnalytics);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio Lab"))
            {
                drawAudioLabPage(audio, audioLevel, autoGain, settings, analysisBands, mappedBands, smoothedBands);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Timeline"))
            {
                drawTimelinePage(runtimeTelemetry);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Firmware"))
            {
                drawFirmwarePage(performance, hasPerformance, timingProbe, hasTimingProbe);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Performance"))
            {
                const std::size_t framebufferPacketBytes = sizeof(PacketHeader) + sizeof(FramebufferSetPayload<MatrixSize>);
                const double configuredTxKiB = settings.Enabled && settings.SendFramebuffer ? framebufferPacketBytes * static_cast<double>(settings.FrameRate) / 1024.0 : 0.0;
                ImGui::Text("Host CPU: %.2f%%   target framebuffer rate: %d Hz   estimated TX: %.1f KiB/s", appCpuUsage, settings.FrameRate, configuredTxKiB);
                ImGui::Text("Packets received: %llu   frames sent: %llu   dropped/busy: %llu", static_cast<unsigned long long>(receivedPackets), static_cast<unsigned long long>(sentFrames), static_cast<unsigned long long>(droppedFrames));
                ImGui::Separator();
                if (hasPerformance)
                    drawPerformance(performance);
                else
                    ImGui::TextDisabled("Waiting for PerformanceResponse...");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Matrix timing"))
            {
                if (hasTimingProbe)
                    drawTimingProbe(timingProbe);
                else
                    ImGui::TextDisabled("Waiting for MatrixTimingProbeResult...");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
}

int main(int argc, char* argv[])
{
    auto hidden = false;
    if (argc >= 2)
    {
        hidden = strcmp(argv[1], "hidden") == 0;
    }
    VisualizerSettings settings;
    loadSettings(settings);

    glfwSetErrorCallback([](const int error, const char* description) { std::fprintf(stderr, "GLFW error %d: %s\n", error, description); });
    if (!glfwInit())
        return EXIT_FAILURE;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Quartz", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwSetWindowCloseCallback(window, [](GLFWwindow* target)
    {
        glfwSetWindowShouldClose(target, GLFW_FALSE);
        glfwHideWindow(target);
    });

    glfwMakeContextCurrent(window);
#ifdef GLFW_LOCK_KEY_MODS
    glfwSetInputMode(window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
#endif
    glfwSwapInterval(0);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)))
    {
        std::fprintf(stderr, "Failed to initialize OpenGL loader\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    applyDarkTheme();
    initializeShaderEditorFonts();

    constexpr const char* GLSLVersion = "#version 330";
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true))
    {
        std::fprintf(stderr, "Failed to initialize ImGui GLFW backend\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (!ImGui_ImplOpenGL3_Init(GLSLVersion))
    {
        std::fprintf(stderr, "Failed to initialize ImGui OpenGL backend\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    RawUSB usb;
    AudioSpectrum audio;
    MediaColorProvider mediaColor;
    ShaderFramebuffer shaderFramebuffer;
    ShaderTransitionState shaderTransition;
    ShaderEditorState shaderEditor;
    ViewPage page = ViewPage::Main;
    std::array<char, ShaderSourceCapacity> vertexShaderSource{};
    std::array<char, ShaderSourceCapacity> fragmentShaderSource{};
    std::array<char, ShaderPathCapacity> vertexLoadPath{};
    std::array<char, ShaderPathCapacity> fragmentLoadPath{};
    loadShaderSources(vertexShaderSource, fragmentShaderSource);
    if (migrateObsoleteShaderSource(fragmentShaderSource))
        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
    const auto defaultVertexPath = vertexShaderPath().string();
    const auto defaultFragmentPath = fragmentShaderPath().string();
    std::snprintf(vertexLoadPath.data(), vertexLoadPath.size(), "%s", defaultVertexPath.c_str());
    std::snprintf(fragmentLoadPath.data(), fragmentLoadPath.size(), "%s", defaultFragmentPath.c_str());
    if (std::string_view(fragmentShaderSource.data()) == LegacyDefaultFragmentShaderSource || std::string_view(fragmentShaderSource.data()) == DefaultFragmentShaderSource)
    {
        setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
    }
    settings.ShaderPresetIndex = detectShaderPreset(fragmentShaderSource.data());
    initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
    SharedDeviceState deviceState;
    std::array<float, FFTSize> analysisBands{};
    std::array<float, Columns> mappedBands{};
    std::array<float, Columns> smoothedBands{};
    std::array<Color32, MatrixSize> framebuffer{};
    std::optional<Color32> visualizerColor;
    float mediaColorAmount = 0.0f;
    std::uint64_t sentFrames = 0;
    std::uint64_t droppedFrames = 0;
    AppCpuMeter appCpuMeter;
    float appCpuUsage = 0.0f;
    EvdevKeyboard keyboardInput;
    ReactiveKeyState reactiveKeys;
    RuntimeInputAnalytics inputAnalytics;
    RuntimeRGBAnalytics rgbAnalytics;
    RuntimeBindingEngine runtimeBindings;
    RuntimeTelemetry runtimeTelemetry;
    AutoGainState autoGain;
    autoGain.reset(settings);
    AudioLevelSnapshot audioLevel{};

    loadShaderMaterialValueCache();
    shaderFramebuffer.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);

    usb.setPacketObserver([&](const bool tx, const PacketHeader& packet, const std::span<const std::byte> bytes)
    {
        runtimeTelemetry.packet(glfwGetTime(), tx, packet, bytes);
    });

    usb.setPacketHandler([&](const PacketHeader& packet)
    {
        if (!packet.isDeviceToHost())
            return;
        std::lock_guard lock(deviceState.Mutex);
        ++deviceState.ReceivedPackets;
        if (packet.Type == PacketType::PerformanceResponse)
        {
            if (const auto* payload = packet.getPayload<PerformancePayload>())
            {
                deviceState.Performance = {
                    payload->CoreClock,
                    payload->BeginScanTicks,
                    payload->ScanTicks,
                    payload->EndScanTicks,
                    payload->StateUpdateTicks,
                    payload->HIDTicks,
                    payload->RGBTicks,
                    payload->AverageScanPeriodTicks,
                    payload->RGBSlotMaxTicks
                };
                deviceState.HasPerformance = true;
            }
        }
        else if (packet.Type == PacketType::MatrixTimingProbeResult)
        {
            if (const auto* payload = packet.getPayload<MatrixTimingProbeResult<ActiveProbeRows>>())
            {
                deviceState.TimingProbe = *payload;
                deviceState.HasTimingProbe = true;
            }
        }
    });

    try
    {
        if (!usb.initialize())
            throw std::runtime_error(libusb_error_name(usb.lastError()));
        usb.connect();
        audio.start(settings.AudioSource);
        mediaColor.start();
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "Quartz initialization failed: %s\n", exception.what());
    }

    const double startTime = glfwGetTime();
    keyboardInput.start(startTime);
    runtimeTelemetry.event(startTime, "Application", "Quartz runtime studio started");
    double nextVisualizerFrame = startTime;
    double nextStatisticsRequest = startTime;
    double nextReconnectAttempt = startTime;
    double nextSettingsSave = startTime + 0.50;
    std::string savedSettings = serializeSettings(settings);
    std::uint64_t savedMaterialRevision = shaderFramebuffer.materialRevision();
    bool lastUSBConnected = usb.isConnected();
    std::string lastMediaTitle = mediaColor.mediaTitle();
    bool lastMediaPlaying = mediaColor.playing();
    bool lastCapsLock = reactiveKeys.CapsLockActive;
    bool lastScrollLock = reactiveKeys.ScrollLockActive;
    if (hidden)
    {
        glfwHideWindow(window);
    }
    while (!glfwWindowShouldClose(window))
    {
        if (keyboardInput.consumeRestoreRequest()) restoreWindow(window);
        const double currentFrame = glfwGetTime();
        appCpuUsage = appCpuMeter.update(currentFrame);
        runtimeBindings.updateRates(usb.stats(), currentFrame);
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        reactiveKeys = keyboardInput.snapshot();
        for (std::size_t i = 0; i < MatrixSize; ++i)
        {
            const bool down = reactiveKeys.Down[i] > 0.5f;
            const bool wasDown = inputAnalytics.Previous[i] > 0.5f;
            if (down != wasDown)
            {
                const std::size_t row = i / Columns, column = i % Columns;
                runtimeTelemetry.event(currentFrame, "Input", "r" + std::to_string(row) + " c" + std::to_string(column) + (down ? " down" : " up"));
            }
        }
        inputAnalytics.update(reactiveKeys, currentFrame);
        const bool scrollLockActive = reactiveKeys.ScrollLockActive;
        const bool capsLockActive = reactiveKeys.CapsLockActive;
        if (capsLockActive != lastCapsLock)
        {
            runtimeTelemetry.event(currentFrame, "Input", std::string("Caps Lock ") + (capsLockActive ? "on" : "off"));
            lastCapsLock = capsLockActive;
        }
        if (scrollLockActive != lastScrollLock)
        {
            runtimeTelemetry.event(currentFrame, "Input", std::string("Scroll Lock ") + (scrollLockActive ? "on" : "off"));
            lastScrollLock = scrollLockActive;
        }

        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderPresetIndex = std::clamp(settings.ShaderPresetIndex, 0, static_cast<int>(ShaderPresets.size()));
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.ShaderTransitionSeconds = std::clamp(settings.ShaderTransitionSeconds, 0.0f, 10.0f);
        settings.AutoGainBaseline = std::clamp(settings.AutoGainBaseline, 0.05f, 20.0f);
        settings.AutoGainTargetRms = std::clamp(settings.AutoGainTargetRms, 0.005f, 0.75f);
        settings.AutoGainAdaptation = std::clamp(settings.AutoGainAdaptation, 0.01f, 5.0f);
        settings.AutoGainMinCorrection = std::clamp(settings.AutoGainMinCorrection, 0.05f, 10.0f);
        settings.AutoGainMaxCorrection = std::clamp(settings.AutoGainMaxCorrection, settings.AutoGainMinCorrection, 20.0f);
        settings.AutoGainSilenceGate = std::clamp(settings.AutoGainSilenceGate, 0.0f, 0.25f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.LiveOutputInterpolation = std::clamp(settings.LiveOutputInterpolation, 0.0f, 1.0f);
        settings.FeatherRows = std::max(settings.FeatherRows, 0.01f);
        settings.MaxFrequency = std::max(settings.MaxFrequency, settings.MinFrequency + 1.0f);
        settings.MaxDb = std::max(settings.MaxDb, settings.MinDb + 0.1f);
        mediaColor.setPollInterval(settings.MediaPollInterval);

        const bool usbConnectedNow = usb.isConnected();
        if (usbConnectedNow != lastUSBConnected)
        {
            runtimeTelemetry.event(currentFrame, "USB", usbConnectedNow ? "Device connected" : "Device disconnected");
            lastUSBConnected = usbConnectedNow;
        }
        const std::string mediaTitleNow = mediaColor.mediaTitle();
        const bool mediaPlayingNow = mediaColor.playing();
        if (mediaTitleNow != lastMediaTitle || mediaPlayingNow != lastMediaPlaying)
        {
            runtimeTelemetry.event(currentFrame, "Media", mediaPlayingNow ? "Playing: " + mediaTitleNow : "Paused/stopped: " + mediaTitleNow);
            lastMediaTitle = mediaTitleNow;
            lastMediaPlaying = mediaPlayingNow;
        }

        if (settings.AutoReconnect && !usb.isConnected() && currentFrame >= nextReconnectAttempt)
        {
            usb.connect();
            nextReconnectAttempt = currentFrame + 1.0;
        }

        const double visualizerFrameTime = 1.0 / std::max(1, settings.FrameRate);
        if (settings.Enabled && currentFrame >= nextVisualizerFrame)
        {
            audioLevel = audio.levelSnapshot();
            autoGain.update(audioLevel, settings, static_cast<float>(visualizerFrameTime));
            audio.getBands(std::span(analysisBands).first(settings.AnalysisBandCount), settings.MinFrequency, settings.MaxFrequency, settings.MinDb, settings.MaxDb);
            mapSpectrumToColumns(std::span<const float>(analysisBands).first(settings.AnalysisBandCount), mappedBands, settings, autoGain.EffectiveGain);
            smoothBands(mappedBands, smoothedBands, settings, static_cast<float>(visualizerFrameTime));

            const float colorAlpha = 1.0f - std::exp(-settings.ColorTransitionSpeed * static_cast<float>(visualizerFrameTime));
            const auto mediaTarget = settings.MediaArtworkColor ? mediaColor.targetColor() : std::nullopt;
            if (mediaTarget)
                visualizerColor = visualizerColor ? lerpColor(*visualizerColor, *mediaTarget, colorAlpha) : mediaTarget;
            const float targetColorAmount = mediaTarget ? 1.0f : 0.0f;
            mediaColorAmount += (targetColorAmount - mediaColorAmount) * colorAlpha;
            if (std::abs(targetColorAmount - mediaColorAmount) < 0.001f)
                mediaColorAmount = targetColorAmount;

            PerformanceSnapshot runtimePerformance{};
            bool runtimeHasPerformance = false;
            {
                std::lock_guard lock(deviceState.Mutex);
                runtimePerformance = deviceState.Performance;
                runtimeHasPerformance = deviceState.HasPerformance;
            }
            RuntimeSignalContext runtimeContext;
            runtimeContext.Time = currentFrame;
            runtimeContext.DeltaTime = static_cast<float>(visualizerFrameTime);
            runtimeContext.Audio = audioLevel;
            runtimeContext.MappedBands = &mappedBands;
            runtimeContext.SmoothedBands = &smoothedBands;
            runtimeContext.MediaColor = visualizerColor;
            runtimeContext.MediaAmount = mediaColorAmount;
            runtimeContext.MediaPlaying = mediaColor.playing();
            runtimeContext.Keys = reactiveKeys;
            runtimeContext.Performance = runtimePerformance;
            runtimeContext.HasPerformance = runtimeHasPerformance;
            runtimeContext.AppCpu = appCpuUsage;
            runtimeContext.USBConnected = usb.isConnected();
            runtimeContext.USB = usb.stats();
            runtimeContext.USBRates = runtimeBindings.usbRates();
            runtimeContext.Framebuffer = &framebuffer;
            runtimeContext.EffectiveGain = autoGain.EffectiveGain;
            runtimeContext.GainCorrection = autoGain.Correction;
            runtimeBindings.update(runtimeContext, shaderFramebuffer);
            const RuntimeControlOutput controlOutput = runtimeBindings.evaluateControls(shaderFramebuffer);
            if (controlOutput.ShaderPresetIndex && *controlOutput.ShaderPresetIndex != settings.ShaderPresetIndex)
            {
                if (switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, *controlOutput.ShaderPresetIndex, currentFrame, controlOutput.ShaderTransitionSeconds, false))
                    runtimeBindings.applyMaterialValues(shaderFramebuffer);
            }

            const int effectiveBaseColorMode = controlOutput.BaseColorMode.value_or(settings.BaseColorMode);
            const float effectiveBrightness = controlOutput.GlobalBrightness.value_or(settings.GlobalBrightness);
            const bool effectiveSendFramebuffer = controlOutput.SendFramebuffer.value_or(settings.SendFramebuffer);
            if (effectiveBaseColorMode == 2)
            {
                if (!shaderFramebuffer.render(currentFrame, smoothedBands, settings, visualizerColor, mediaColorAmount, reactiveKeys, framebuffer))
                    framebuffer.fill({0, 0, 0});
                if (shaderTransition.Active)
                {
                    const float duration = std::max(shaderTransition.Duration, 0.0001f);
                    const float linear = std::clamp(static_cast<float>((currentFrame - shaderTransition.StartedAt) / duration), 0.0f, 1.0f);
                    const float blend = linear * linear * (3.0f - 2.0f * linear);
                    if (shaderTransition.Previous.render(currentFrame, smoothedBands, settings, visualizerColor, mediaColorAmount, reactiveKeys, shaderTransition.Frame))
                        for (std::size_t i = 0; i < MatrixSize; ++i) framebuffer[i] = lerpColorLinear(shaderTransition.Frame[i], framebuffer[i], blend);
                    if (linear >= 1.0f) shaderTransition.cancel();
                }
            }
            else
                renderAudioRGB(framebuffer, smoothedBands, settings, visualizerColor, mediaColorAmount, static_cast<float>(currentFrame * settings.WaveSpeed));
            applyGlobalBrightness(framebuffer, effectiveBrightness);
            rgbAnalytics.update(framebuffer, currentFrame);
            if (effectiveSendFramebuffer && usb.isConnected())
            {
                if (sendFramebuffer(usb, framebuffer))
                    ++sentFrames;
                else
                    ++droppedFrames;
            }

            nextVisualizerFrame += visualizerFrameTime;
            if (currentFrame - nextVisualizerFrame > visualizerFrameTime)
                nextVisualizerFrame = currentFrame + visualizerFrameTime;
        }

        if (usb.isConnected() && currentFrame >= nextStatisticsRequest)
        {
            usb.send(makePacket(PacketType::PerformanceRequest));
            nextStatisticsRequest = currentFrame + std::max(0.05f, settings.StatisticsInterval);
        }

        drawUi(usb, audio, mediaColor, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, analysisBands, mappedBands, smoothedBands, framebuffer, deviceState, runtimeBindings, runtimeTelemetry, autoGain, audioLevel, reactiveKeys, inputAnalytics, rgbAnalytics, sentFrames, droppedFrames, appCpuUsage, scrollLockActive, capsLockActive);
        if (currentFrame >= nextSettingsSave)
        {
            const std::string currentSettings = serializeSettings(settings);
            if (currentSettings != savedSettings && saveSettings(settings))
                savedSettings = currentSettings;
            if (shaderFramebuffer.materialRevision() != savedMaterialRevision && shaderFramebuffer.saveMaterialValues())
                savedMaterialRevision = shaderFramebuffer.materialRevision();
            runtimeBindings.saveIfChanged();
            nextSettingsSave = currentFrame + 0.50;
        }
        ImGui::Render();

        int width;
        int height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (settings.LimitMainLoop)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
    }

    saveSettings(settings);
    saveShaderSources(vertexShaderSource, fragmentShaderSource);
    shaderFramebuffer.saveMaterialValues();
    runtimeBindings.save();
    keyboardInput.stop();
    shaderTransition.cancel();
    shaderFramebuffer.shutdown();
    usb.shutdown();
    mediaColor.stop();
    audio.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
