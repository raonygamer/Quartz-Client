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

#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


// clang-format off
#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#undef GLAD_GL_IMPLEMENTATION
#include <GLFW/glfw3.h>
// clang-format on

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
    float t = uTime * 0.9;
    float value = sin(p.x * 1.7 + t)
        + sin(p.y * 2.1 - t * 1.3)
        + sin((p.x + p.y) * 1.4 + t * 0.7)
        + sin(length(p) * 2.4 - t * 1.8);
    value = value * 0.125 + 0.5;

    vec3 color = hsv2rgb(vec3(fract(value + t * 0.035), 0.95, 1.0));
    color = mix(color, uMediaColor, uMediaAmount * 0.55);
    FragColor = vec4(applyKeyIndicators(color), 1.0);
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

    static std::vector<ShaderPreset> buildShaderPresets()
    {
        std::vector<ShaderPreset> presets;
        presets.reserve(57);
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
        float GlobalBrightness = 1.0f;
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
        setShaderSource(fragmentSource, DefaultFragmentShaderSource);
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
        stream << "GlobalBrightness=" << settings.GlobalBrightness << '\n';
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
            LOAD_NUM(GlobalBrightness)
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
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
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

            if (_program != 0)
                glDeleteProgram(_program);
            _program = program;
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

    private:
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

    class RawUSB
    {
    public:
        using PacketHandler = std::function<void(const PacketHeader&)>;

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

        bool send(const PacketBuffer& packet) noexcept
        {
            if (!_handle || !_connected.load(std::memory_order_acquire) || packet.Size == 0 || packet.Size > packet.Data.size())
                return false;
            int transferred = 0;
            const int result = libusb_bulk_transfer(_handle, RequestEndpoint, reinterpret_cast<unsigned char*>(const_cast<std::byte*>(packet.Data.data())), static_cast<int>(packet.Size), &transferred, 20);
            _lastError.store(result, std::memory_order_release);
            if (result == LIBUSB_ERROR_NO_DEVICE)
            {
                _running.store(false, std::memory_order_release);
                _connected.store(false, std::memory_order_release);
            }
            return result == LIBUSB_SUCCESS && transferred == static_cast<int>(packet.Size);
        }

    private:
        static constexpr int RPCInterface = 1;
        static constexpr unsigned char RequestEndpoint = 0x03;
        static constexpr unsigned char ResponseEndpoint = 0x84;

        void _receiveLoop() noexcept
        {
            std::array<unsigned char, 512> buffer{};
            while (_running.load(std::memory_order_acquire))
            {
                int transferred = 0;
                const int result = libusb_bulk_transfer(_handle, ResponseEndpoint, buffer.data(), static_cast<int>(buffer.size()), &transferred, 100);
                if (result == LIBUSB_ERROR_TIMEOUT)
                    continue;
                _lastError.store(result, std::memory_order_release);
                if (result != LIBUSB_SUCCESS)
                {
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
        PacketHandler _packetHandler;
        std::vector<std::byte> _rxAssembly;
        std::string _deviceName;
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
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus(discovery.Status);
                return;
            }
            const auto& media = *discovery.Media;
            if (media.Status != "Playing")
            {
                _lastMediaKey.clear();
                _targetColor.store(-1, std::memory_order_release);
                setStatus("MPRIS " + (media.Status.empty() ? std::string("not playing") : media.Status) + " (" + media.Player + ")", media.Title);
                return;
            }
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
        mutable std::mutex _stateMutex;
        std::string _lastMediaKey;
        std::string _mediaTitle;
        std::string _status = "Starting";
    };

    static void mapSpectrumToColumns(const std::span<const float> analysisBands, std::array<float, Columns>& bands, const VisualizerSettings& settings)
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
            float gain = settings.ColumnGain[column] * settings.OverallGain;
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

    static void drawFramebufferPreview(const std::array<Color32, MatrixSize>& framebuffer, const float widthFraction = 0.55f, const float maxWidth = 532.0f)
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
            const auto& color = framebuffer[key.Row * Columns + key.Column];
            // Keep actual RGB values unchanged for lit keys. Pure black framebuffer cells
            // get a charcoal preview fill so the physical keyboard silhouette remains visible.
            const ImU32 previewColor = color.R == 0 && color.G == 0 && color.B == 0 ? IM_COL32(22, 22, 22, 255) : IM_COL32(color.R, color.G, color.B, 255);
            const ImVec2 min(origin.x + key.X * unit + gap, origin.y + key.Y * unit + gap);
            const ImVec2 max(origin.x + (key.X + key.Width) * unit - gap, origin.y + (key.Y + key.Height) * unit - gap);
            drawList->AddRectFilled(min, max, previewColor, 0.0f);
        }

        ImGui::Dummy(ImVec2(LayoutWidth * unit, LayoutHeight * unit));
    }

    static void drawShaderLivePanel(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive, const VisualizerSettings& settings)
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
            drawFramebufferPreview(framebuffer, 1.0f, 410.0f);

            ImGui::TableNextColumn();
            ImGui::Text("Current shader: %s", settings.ShaderPresetIndex == 0 ? "Custom / current" : ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str());
            ImGui::Text("VID:PID: %04X:%04X", VendorId, ProductId);
            ImGui::Text("Global brightness: %.0f%%", settings.GlobalBrightness * 100.0f);
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

    static void drawShaderEditorPage(RawUSB& usb, SharedDeviceState& deviceState, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<Color32, MatrixSize>& framebuffer, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
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
                    settings.ShaderPresetIndex = static_cast<int>(i + 1);
                    setShaderSource(fragmentShaderSource, ShaderPresets[i].FragmentSource);
                    shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
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
                    setShaderSource(fragmentShaderSource, DefaultFragmentShaderSource);
                    shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                    saveShaderSources(vertexShaderSource, fragmentShaderSource);
                }
                const float previewReserve = 268.0f;
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
                const float previewReserve = 268.0f;
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
        ImGui::Text("Quartz K552X  |  USB %s  |  %04X:%04X", usb.isConnected() ? "connected" : "disconnected", VendorId, ProductId);
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide window and icon")) hideWindow(glfwGetCurrentContext());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore globally with Ctrl + Alt + Shift + Q");
        const char* credits = "Made by Raony Reis, not affiliated with Redragon";
        const float creditWidth = ImGui::CalcTextSize(credits).x;
        const float right = ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetCursorPosX() + creditWidth + 16.0f < right) ImGui::SameLine(right - creditWidth);
        ImGui::TextDisabled("%s", credits);
        ImGui::Separator();
    }

    static void drawUi(RawUSB& usb, AudioSpectrum& audio, MediaColorProvider& mediaColor, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderEditorState& shaderEditor, ViewPage& page, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands, const std::array<Color32, MatrixSize>& framebuffer, SharedDeviceState& deviceState, std::uint64_t sentFrames, std::uint64_t droppedFrames, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Quartz K552X Visualizer", nullptr, windowFlags);
        drawPermanentHeader(usb);
        if (page == ViewPage::ShaderEditor)
        {
            drawShaderEditorPage(usb, deviceState, keyboardInput, shaderFramebuffer, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, framebuffer, appCpuUsage, scrollLockActive, capsLockActive);
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
                ImGui::SliderFloat("Overall gain", &settings.OverallGain, 0.10f, 4.00f, "%.2fx");
                defaultButton("OverallGain", settings.OverallGain, defaults.OverallGain);
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
                                settings.ShaderPresetIndex = static_cast<int>(i + 1);
                                setShaderSource(fragmentShaderSource, ShaderPresets[i].FragmentSource);
                                if (shaderEditor.Initialized) shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                                compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                                saveShaderSources(vertexShaderSource, fragmentShaderSource);
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (defaultButton("ShaderPresetIndex", settings.ShaderPresetIndex, defaults.ShaderPresetIndex))
                    {
                        const int presetIndex = std::clamp(settings.ShaderPresetIndex - 1, 0, static_cast<int>(ShaderPresets.size()) - 1);
                        setShaderSource(fragmentShaderSource, ShaderPresets[presetIndex].FragmentSource);
                        if (shaderEditor.Initialized) shaderEditor.Fragment.SetText(fragmentShaderSource.data());
                        compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);
                        saveShaderSources(vertexShaderSource, fragmentShaderSource);
                    }
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
                        setShaderSource(fragmentShaderSource, DefaultFragmentShaderSource);
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
                    drawFramebufferPreview(framebuffer);
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

            if (ImGui::BeginTabItem("Performance"))
            {
                ImGui::Text("Packets received: %llu", static_cast<unsigned long long>(receivedPackets));
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
    if (std::string_view(fragmentShaderSource.data()) == LegacyDefaultFragmentShaderSource)
    {
        setShaderSource(fragmentShaderSource, DefaultFragmentShaderSource);
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

    shaderFramebuffer.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);

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
    double nextVisualizerFrame = startTime;
    double nextStatisticsRequest = startTime;
    double nextReconnectAttempt = startTime;
    double nextSettingsSave = startTime + 0.50;
    std::string savedSettings = serializeSettings(settings);
    if (hidden)
    {
        glfwHideWindow(window);
    }
    while (!glfwWindowShouldClose(window))
    {
        if (keyboardInput.consumeRestoreRequest()) restoreWindow(window);
        const double currentFrame = glfwGetTime();
        appCpuUsage = appCpuMeter.update(currentFrame);
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        reactiveKeys = keyboardInput.snapshot();
        const bool scrollLockActive = reactiveKeys.ScrollLockActive;
        const bool capsLockActive = reactiveKeys.CapsLockActive;

        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderPresetIndex = std::clamp(settings.ShaderPresetIndex, 0, static_cast<int>(ShaderPresets.size()));
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.FeatherRows = std::max(settings.FeatherRows, 0.01f);
        settings.MaxFrequency = std::max(settings.MaxFrequency, settings.MinFrequency + 1.0f);
        settings.MaxDb = std::max(settings.MaxDb, settings.MinDb + 0.1f);
        mediaColor.setPollInterval(settings.MediaPollInterval);

        if (settings.AutoReconnect && !usb.isConnected() && currentFrame >= nextReconnectAttempt)
        {
            usb.connect();
            nextReconnectAttempt = currentFrame + 1.0;
        }

        const double visualizerFrameTime = 1.0 / std::max(1, settings.FrameRate);
        if (settings.Enabled && currentFrame >= nextVisualizerFrame)
        {
            audio.getBands(std::span(analysisBands).first(settings.AnalysisBandCount), settings.MinFrequency, settings.MaxFrequency, settings.MinDb, settings.MaxDb);
            mapSpectrumToColumns(std::span<const float>(analysisBands).first(settings.AnalysisBandCount), mappedBands, settings);
            smoothBands(mappedBands, smoothedBands, settings, static_cast<float>(visualizerFrameTime));

            const float colorAlpha = 1.0f - std::exp(-settings.ColorTransitionSpeed * static_cast<float>(visualizerFrameTime));
            const auto mediaTarget = settings.MediaArtworkColor ? mediaColor.targetColor() : std::nullopt;
            if (mediaTarget)
                visualizerColor = visualizerColor ? lerpColor(*visualizerColor, *mediaTarget, colorAlpha) : mediaTarget;
            const float targetColorAmount = mediaTarget ? 1.0f : 0.0f;
            mediaColorAmount += (targetColorAmount - mediaColorAmount) * colorAlpha;
            if (std::abs(targetColorAmount - mediaColorAmount) < 0.001f)
                mediaColorAmount = targetColorAmount;

            if (settings.BaseColorMode == 2)
            {
                if (!shaderFramebuffer.render(currentFrame, smoothedBands, settings, visualizerColor, mediaColorAmount, reactiveKeys, framebuffer))
                    framebuffer.fill({0, 0, 0});
            }
            else
                renderAudioRGB(framebuffer, smoothedBands, settings, visualizerColor, mediaColorAmount, static_cast<float>(currentFrame * settings.WaveSpeed));
            applyGlobalBrightness(framebuffer, settings.GlobalBrightness);
            if (settings.SendFramebuffer && usb.isConnected())
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

        drawUi(usb, audio, mediaColor, keyboardInput, shaderFramebuffer, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, analysisBands, mappedBands, smoothedBands, framebuffer, deviceState, sentFrames, droppedFrames, appCpuUsage, scrollLockActive, capsLockActive);
        if (currentFrame >= nextSettingsSave)
        {
            const std::string currentSettings = serializeSettings(settings);
            if (currentSettings != savedSettings && saveSettings(settings))
                savedSettings = currentSettings;
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
    keyboardInput.stop();
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
