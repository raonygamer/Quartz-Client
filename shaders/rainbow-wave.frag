#version 330 core
// @shader id="custom.rainbow_wave" label="Rainbow wave"

in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform vec3 uMediaColor;
uniform float uMediaAmount;
uniform float uCapsLock;
uniform float uScrollLock;
uniform vec3 uCapsLockColor;
uniform vec3 uScrollLockColor;
uniform int uCapsLockColorEnabled;
uniform int uScrollLockColorEnabled;

uniform float uRainbowAngle; // @ui min=-180 max=180 step=1 default=0 label="Angle (degrees)" id=rainbow_wave.angle
uniform float uRainbowSpeed; // @ui min=-2 max=2 step=0.01 default=0.12 label="Speed (cycles/sec)" id=rainbow_wave.speed
uniform float uRainbowPhase; // @ui min=0 max=1 step=0.01 default=0 label="Phase (cycles)" id=rainbow_wave.phase
uniform float uRainbowDensity; // @ui min=0.1 max=8 step=0.05 default=1 label="Density (cycles/width)" id=rainbow_wave.density
uniform float uRainbowSaturation; // @ui min=0 max=1 step=0.01 default=1 label="Saturation" id=rainbow_wave.saturation
uniform float uRainbowBrightness; // @ui min=0 max=1 step=0.01 default=1 label="Brightness" id=rainbow_wave.brightness
uniform float uRainbowBend; // @ui min=0 max=1 step=0.01 default=0 label="Wave bend" id=rainbow_wave.bend
uniform float uRainbowBendFrequency; // @ui min=0.1 max=8 step=0.1 default=2 label="Bend frequency" id=rainbow_wave.bend_frequency
uniform float uRainbowMediaTint; // @ui min=0 max=1 step=0.01 default=0 label="Media color blend" id=rainbow_wave.media_tint

vec3 hsvToRgb(float hue, float saturation)
{
    vec3 channels = abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return mix(vec3(1.0), clamp(channels - 1.0, 0.0, 1.0), saturation);
}

void main()
{
    vec2 keyPosition = vec2(vUV.x * 16.0, (1.0 - vUV.y) * 7.0);
    int column = clamp(int(floor(keyPosition.x)), 0, 15);
    int row = clamp(int(floor(keyPosition.y)), 0, 6);
    if (row == 6)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Both axes use key spacing, so rotation keeps the wave's physical spacing.
    vec2 position = (keyPosition - vec2(8.0, 3.0)) / 16.0;
    float angle = radians(uRainbowAngle);
    vec2 direction = vec2(cos(angle), sin(angle));
    vec2 across = vec2(-direction.y, direction.x);
    float bend = uRainbowBend * sin(6.28318530718 * dot(position, across) * uRainbowBendFrequency);
    float hue = dot(position, direction) * uRainbowDensity + bend
        - uTime * uRainbowSpeed + uRainbowPhase;

    vec3 color = hsvToRgb(fract(hue), clamp(uRainbowSaturation, 0.0, 1.0));
    color = mix(color, uMediaColor, clamp(uRainbowMediaTint * uMediaAmount, 0.0, 1.0));
    color *= clamp(uRainbowBrightness, 0.0, 1.0);

    if (uCapsLockColorEnabled != 0 && uCapsLock > 0.5 && row == 3 && column == 0)
        color = uCapsLockColor;
    if (uScrollLockColorEnabled != 0 && uScrollLock > 0.5 && row == 0 && column == 14)
        color = uScrollLockColor;
    FragColor = vec4(color, 1.0);
}
