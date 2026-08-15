#include "quartz/client/Model.hpp"

namespace quartz::client
{
    const std::string_view DefaultVertexShaderSource = R"GLSL(#version 330 core
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

    const std::string_view LegacyDefaultFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view DefaultFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view PlasmaFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view NeonRingsFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view RotatingBoxesFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view CheckerWarpFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view DiamondTunnelFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view WaveInterferenceFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view RadialPulseFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view SpectrumFireFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view KaleidoscopeFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view GeometricGridFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view RotatingCubeFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveKeyGlowFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveRippleFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveRainbowRippleFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveHeatFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveCrossBlastFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveSparksFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveManhattanFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveVortexFragmentShaderSource = R"GLSL(#version 330 core
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

    const std::string_view ReactiveStarburstFragmentShaderSource = R"GLSL(#version 330 core
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
    const std::string_view ReactiveScannerFragmentShaderSource = R"GLSL(#version 330 core
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

    std::string makeGeneratedShader(const std::string_view body)
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

    std::string shaderPresetId(const std::string_view name)
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

    void parameterizeShaderPreset(ShaderPreset& preset)
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

    std::string makeSourceShader(const std::string_view declaration, const std::string_view body)
    {
        std::string source = makeGeneratedShader(body);
        const std::size_t marker = source.find("uniform float uTime;");
        if (marker != std::string::npos) source.insert(marker, std::string(declaration) + "\n");
        return source;
    }

    std::vector<ShaderPreset> buildShaderPresets()
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
        for (auto& preset : presets)
        {
            parameterizeShaderPreset(preset);
            preset.Id = "builtin." + shaderPresetId(preset.Name);
            preset.BuiltIn = true;
        }
        return presets;
    }

}
