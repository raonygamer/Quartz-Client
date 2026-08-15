from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
inc = root / "include/quartz/client"
model_path = inc / "Model.hpp"

mapping = {
    "ShaderPreset": "shader/ShaderPreset.hpp",
    "PerformanceSnapshot": "device/DeviceState.hpp",
    "SharedDeviceState": "device/DeviceState.hpp",
    "ReactiveKeyBinding": "input/Input.hpp",
    "ReactiveKeyEvent": "input/Input.hpp",
    "ReactiveKeyState": "input/Input.hpp",
    "EvdevKeyboard": "input/Input.hpp",
    "AppCpuMeter": "platform/AppCpuMeter.hpp",
    "VisualizerSettings": "settings/VisualizerSettings.hpp",
    "ViewPage": "shader/ShaderEditor.hpp",
    "ShaderEditorState": "shader/ShaderEditor.hpp",
    "HSV": "shader/ShaderMaterial.hpp",
    "ShaderUniformMetadata": "shader/ShaderMaterial.hpp",
    "ShaderMaterialParameter": "shader/ShaderMaterial.hpp",
    "ShaderFramebuffer": "shader/ShaderFramebuffer.hpp",
    "ShaderTransitionState": "shader/ShaderTransition.hpp",
    "AudioLevelSnapshot": "audio/Audio.hpp",
    "AudioSpectrum": "audio/Audio.hpp",
    "AutoGainState": "audio/Audio.hpp",
    "AudioSourceInfo": "audio/Audio.hpp",
    "MediaColorProvider": "media/Media.hpp",
    "ProcessValueType": "native/NativeTypes.hpp",
    "ProcessAddressMode": "native/NativeTypes.hpp",
    "RuntimeSignaturePatternKind": "native/NativeTypes.hpp",
    "SignatureResultMode": "native/NativeTypes.hpp",
    "RuntimeX64Register": "native/NativeTypes.hpp",
    "RuntimeDisplacementType": "native/NativeTypes.hpp",
    "ProcessRebindMode": "native/NativeTypes.hpp",
    "RuntimeProcessInfo": "native/NativeTypes.hpp",
    "RuntimeProcessModule": "native/NativeTypes.hpp",
    "RuntimeProcessRegion": "native/NativeTypes.hpp",
    "RuntimeRegisterCaptureState": "native/NativeTypes.hpp",
    "RuntimeBindingProfile": "runtime/Profile.hpp",
    "RuntimeSourceKind": "runtime/RuntimeTypes.hpp",
    "RuntimeParameterSlot": "runtime/RuntimeTypes.hpp",
    "RuntimeControlCondition": "runtime/RuntimeTypes.hpp",
    "RuntimeControlTarget": "runtime/RuntimeTypes.hpp",
    "RuntimeReferenceKind": "runtime/RuntimeTypes.hpp",
    "RuntimeAggregateOperation": "runtime/RuntimeTypes.hpp",
    "RuntimeCompareCondition": "runtime/RuntimeTypes.hpp",
    "RuntimeMassCompareResult": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectFieldType": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectPacking": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectAlignment": "runtime/RuntimeTypes.hpp",
    "RuntimeBankValueType": "runtime/RuntimeTypes.hpp",
    "RuntimeActionTarget": "runtime/RuntimeTypes.hpp",
    "RuntimeBindingOperation": "runtime/RuntimeTypes.hpp",
    "RuntimeActionValueMode": "runtime/RuntimeTypes.hpp",
    "RuntimeActionWhen": "runtime/RuntimeTypes.hpp",
    "RuntimeAction": "runtime/RuntimeTypes.hpp",
    "RuntimeValueBankEntry": "runtime/RuntimeTypes.hpp",
    "RuntimeParameterLink": "runtime/RuntimeTypes.hpp",
    "RuntimeSourceReference": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectField": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectDescriptor": "runtime/RuntimeTypes.hpp",
    "RuntimeObjectPointer": "runtime/RuntimeTypes.hpp",
    "RuntimeBinding": "runtime/RuntimeTypes.hpp",
    "RuntimeControlRule": "runtime/RuntimeTypes.hpp",
    "RuntimeControlOutput": "runtime/RuntimeTypes.hpp",
    "RuntimeTimelineEvent": "runtime/RuntimeTypes.hpp",
    "RuntimePacketRecord": "runtime/RuntimeTypes.hpp",
    "RuntimeUSBRates": "runtime/RuntimeTypes.hpp",
    "RuntimeSignalContext": "runtime/RuntimeTypes.hpp",
    "RuntimeInputAnalytics": "runtime/RuntimeTypes.hpp",
    "RuntimeRGBAnalytics": "runtime/RuntimeTypes.hpp",
    "RuntimeBindingEngine": "runtime/RuntimeBindingEngine.hpp",
    "RuntimeTelemetry": "runtime/RuntimeTelemetry.hpp",
    "PreviewRect": "ui/UIState.hpp",
    "RuntimeMemoryInspectorState": "ui/UIState.hpp",
}

deps = {
    "shader/ShaderPreset.hpp": ['"quartz/client/Functions.hpp"'],
    "device/DeviceState.hpp": ['"quartz/client/Functions.hpp"'],
    "input/Input.hpp": ['"quartz/client/Functions.hpp"'],
    "platform/AppCpuMeter.hpp": ['"quartz/client/Functions.hpp"'],
    "settings/VisualizerSettings.hpp": ['"quartz/client/Functions.hpp"'],
    "shader/ShaderEditor.hpp": ['"quartz/client/Functions.hpp"'],
    "shader/ShaderMaterial.hpp": ['"quartz/client/Functions.hpp"'],
    "shader/ShaderFramebuffer.hpp": [
        '"quartz/client/Functions.hpp"', '"quartz/client/settings/VisualizerSettings.hpp"',
        '"quartz/client/input/Input.hpp"', '"quartz/client/shader/ShaderMaterial.hpp"'],
    "shader/ShaderTransition.hpp": ['"quartz/client/shader/ShaderFramebuffer.hpp"'],
    "audio/Audio.hpp": ['"quartz/client/Functions.hpp"', '"quartz/client/settings/VisualizerSettings.hpp"'],
    "media/Media.hpp": ['"quartz/client/Functions.hpp"'],
    "native/NativeTypes.hpp": ['"quartz/client/Functions.hpp"'],
    "runtime/Profile.hpp": ['"quartz/client/Functions.hpp"'],
    "runtime/RuntimeTypes.hpp": [
        '"quartz/client/Functions.hpp"', '"quartz/client/native/NativeTypes.hpp"', '"quartz/client/runtime/Profile.hpp"',
        '"quartz/client/device/DeviceState.hpp"', '"quartz/client/input/Input.hpp"', '"quartz/client/audio/Audio.hpp"',
        '"quartz/client/usb/USB.hpp"'],
    "runtime/RuntimeBindingEngine.hpp": [
        '"quartz/client/runtime/RuntimeTypes.hpp"', '"quartz/client/shader/ShaderFramebuffer.hpp"'],
    "runtime/RuntimeTelemetry.hpp": ['"quartz/client/runtime/RuntimeTypes.hpp"'],
    "ui/UIState.hpp": ['"quartz/client/runtime/RuntimeBindingEngine.hpp"'],
}

text = model_path.read_text()
marker = "namespace quartz::client\n{\n"
ns_start = text.find(marker)
ns_end = text.rfind("\n}\n")
if ns_start < 0 or ns_end < 0:
    raise SystemExit("could not locate Model.hpp namespace")
preamble = text[:ns_start]
body = text[ns_start + len(marker):ns_end]
closing = text[ns_end:]
lines = body.splitlines(keepends=True)

# The generated model is formatted with four spaces at namespace scope and eight or more inside a declaration.
def starts(lines):
    result = []
    pending_template = None
    pattern = re.compile(r"(?:struct|class|enum class|static_assert\s*\(|inline constexpr\s+|constexpr\s+|template\s*<)")
    for i, line in enumerate(lines):
        if not line.startswith("    ") or line.startswith("        "):
            continue
        stripped = line.strip()
        if stripped.startswith(("template<", "template <")):
            pending_template = i
            result.append(i)
            continue
        if pattern.match(stripped):
            result.append(i)
            pending_template = None
    return sorted(set(result))

indices = starts(lines)
blocks = []
for n, start in enumerate(indices):
    end = indices[n + 1] if n + 1 < len(indices) else len(lines)
    blocks.append("".join(lines[start:end]))
prefix = "".join(lines[:indices[0]]) if indices else body

name_re = re.compile(r"^\s{4}(?:struct|class|enum class)\s+([A-Za-z_][A-Za-z0-9_]*)", re.M)
collected = {path: [] for path in deps}
remaining = [prefix] if prefix else []
seen = set()
for block in blocks:
    match = name_re.search(block)
    name = match.group(1) if match else None
    destination = mapping.get(name) if name else None
    if destination:
        collected[destination].append(block)
        seen.add(name)
    else:
        remaining.append(block)

missing = sorted(set(mapping) - seen)
if missing:
    raise SystemExit("semantic type blocks not found: " + ", ".join(missing))

for relative, blocks_for_header in collected.items():
    path = inc / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    include_lines = "\n".join(f"#include {dep}" for dep in deps[relative])
    path.write_text(f"#pragma once\n{include_lines}\n\nnamespace quartz::client\n{{\n" + "".join(blocks_for_header) + "}\n")

semantic_includes = [
    "shader/ShaderPreset.hpp", "device/DeviceState.hpp", "input/Input.hpp", "platform/AppCpuMeter.hpp",
    "settings/VisualizerSettings.hpp", "shader/ShaderEditor.hpp", "shader/ShaderMaterial.hpp", "shader/ShaderFramebuffer.hpp",
    "shader/ShaderTransition.hpp", "usb/USB.hpp", "audio/Audio.hpp", "media/Media.hpp", "native/NativeTypes.hpp",
    "runtime/Profile.hpp", "runtime/RuntimeTypes.hpp", "runtime/RuntimeBindingEngine.hpp", "runtime/RuntimeTelemetry.hpp", "ui/UIState.hpp"
]
preamble = '#pragma once\n#include "Functions.hpp"\n' + "".join(f'#include "quartz/client/{item}"\n' for item in semantic_includes) + "\n"
model_path.write_text(preamble + marker + "".join(remaining) + closing)

# Replace the broad first-pass facade headers with useful semantic umbrellas.
facades = {
    "shader/Shader.hpp": ["ShaderPreset.hpp", "ShaderEditor.hpp", "ShaderMaterial.hpp", "ShaderFramebuffer.hpp", "ShaderTransition.hpp"],
    "runtime/Runtime.hpp": ["Profile.hpp", "RuntimeTypes.hpp", "RuntimeBindingEngine.hpp", "RuntimeTelemetry.hpp"],
}
for relative, headers in facades.items():
    path = inc / relative
    path.write_text("#pragma once\n" + "".join(f'#include "{header}"\n' for header in headers))

print(f"moved {len(seen)} model types into {len(collected)} semantic subsystem headers")
