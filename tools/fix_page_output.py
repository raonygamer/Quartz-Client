from pathlib import Path

root = Path(__file__).resolve().parents[1]
header = root / "include/quartz/client/ui/PageContext.hpp"

renames = {
    "Usb": "usb",
    "Audio": "audio",
    "MediaColor": "mediaColor",
    "KeyboardInput": "keyboardInput",
    "ShaderFramebuffer": "shaderFramebuffer",
    "ShaderTransition": "shaderTransition",
    "ShaderEditor": "shaderEditor",
    "VertexShaderSource": "vertexShaderSource",
    "FragmentShaderSource": "fragmentShaderSource",
    "VertexLoadPath": "vertexLoadPath",
    "FragmentLoadPath": "fragmentLoadPath",
    "Settings": "settings",
    "AnalysisBands": "analysisBands",
    "MappedBands": "mappedBands",
    "SmoothedBands": "smoothedBands",
    "Framebuffer": "framebuffer",
    "DeviceState": "deviceState",
    "RuntimeBindings": "runtimeBindings",
    "RuntimeTelemetryState": "runtimeTelemetry",
    "AutoGain": "autoGain",
    "AudioLevel": "audioLevel",
    "ReactiveKeys": "reactiveKeys",
    "InputAnalytics": "inputAnalytics",
    "RGBAnalytics": "rgbAnalytics",
    "SentFrames": "sentFrames",
    "DroppedFrames": "droppedFrames",
    "AppCpuUsage": "appCpuUsage",
    "ScrollLockActive": "scrollLockActive",
    "CapsLockActive": "capsLockActive",
}

text = header.read_text()
for old, new in renames.items():
    text = text.replace(f"& {old};", f"& {new};")
    text = text.replace(f" {old} =", f" {new} =")
header.write_text(text)

for path in list((root / "src/ui/pages").glob("*.cpp")) + [root / "src/ui/PageManager.cpp", root / "src/ui/UI.cpp"]:
    if not path.exists(): continue
    text = path.read_text()
    for old, new in renames.items():
        text = text.replace(f"context.{old}", f"context.{new}")
    path.write_text(text)

print("normalized PageContext member names")
