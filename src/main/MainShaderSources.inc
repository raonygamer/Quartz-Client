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
