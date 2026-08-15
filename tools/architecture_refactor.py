from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
INC = SRC / "main"
INCLUDE = ROOT / "include" / "quartz" / "client"

fragments = [
    "MainShaderSources.inc", "MainShaderSources02.inc", "MainInput.inc", "MainInput02.inc", "MainMedia.inc",
    "MainRuntimeModel.inc", "MainRuntimeNative.inc", "MainRuntimeModel02.inc", "MainRuntimeBindings.inc",
    "MainRuntimeControls.inc", "MainRuntimeModel03.inc", "MainRuntimeBindings02.inc", "MainUI.inc"
]

# Main.cpp used to provide every dependency to one anonymous TU. For the first architectural cut we deliberately
# centralize the dependency surface here; subsystem-specific include trimming comes after the code is link-safe.
COMMON = r'''#pragma once
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
'''

TYPE_RE = re.compile(r"^    (struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)")
ENUM_RE = re.compile(r"^    enum\s+class\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^\s{]+))?")
FUNC_RE = re.compile(r"^    (?:static\s+)?(?:inline\s+)?(?:[A-Za-z_:][^;={}]*?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")
GLOBAL_RE = re.compile(r"^    static\s+(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\{|;)")

# Numeric/common constants are now in Common.hpp and must not be duplicated from MainShaderSources.inc.
COMMON_CONSTANTS = {
    "VendorId", "ProductId", "ProtocolVersion", "RPCInterfaceNumber", "RPCOutEndpoint", "RPCInEndpoint",
    "FirmwareVersion", "Rows", "Columns", "MatrixSize", "FFTSize", "ActiveProbeRows", "DefaultShaderWidth",
    "DefaultShaderHeight", "MaxShaderDimension", "ShaderSourceCapacity", "ShaderPathCapacity", "Pi"
}


def top_level_starts(lines: list[str]) -> list[int]:
    starts = []
    pending_template = None
    for i, line in enumerate(lines):
        if not line.startswith("    ") or line.startswith("        "):
            continue
        s = line.strip()
        if s.startswith("template<") or s.startswith("template <"):
            pending_template = i
            continue
        if re.match(r"(?:struct|class|enum class|using\s+|static_assert\s*\(|constexpr\s+|static\s+|inline\s+|[A-Za-z_:~][A-Za-z0-9_:<>, &*\[\]]*\s+[A-Za-z_][A-Za-z0-9_]*\s*\()", s):
            starts.append(pending_template if pending_template is not None else i)
            pending_template = None
    return sorted(set(starts))


def blocks_for(path: Path) -> list[str]:
    lines = path.read_text().splitlines(keepends=True)
    starts = top_level_starts(lines)
    if not starts:
        return ["".join(lines)]
    blocks = []
    for n, start in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        blocks.append("".join(lines[start:end]))
    prefix = "".join(lines[:starts[0]])
    if prefix.strip(): blocks.insert(0, prefix)
    return blocks


def first_code_line(block: str) -> str:
    for line in block.splitlines():
        s = line.strip()
        if s and not s.startswith("//"):
            if s.startswith("template<") or s.startswith("template <"):
                continue
            return line
    return ""


def is_type(block: str) -> bool:
    line = first_code_line(block)
    s = line.strip()
    return s.startswith("struct ") or s.startswith("class ") or s.startswith("enum class ") or s.startswith("using ")


def is_template(block: str) -> bool:
    return any(line.strip().startswith(("template<", "template <")) for line in block.splitlines()[:3])


def is_static_assert(block: str) -> bool:
    return first_code_line(block).strip().startswith("static_assert")


def constexpr_name(block: str) -> str | None:
    s = first_code_line(block).strip()
    if not s.startswith("constexpr "):
        return None
    m = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*=", s)
    return m.group(1) if m else None


def is_shader_source_constant(block: str) -> bool:
    s = first_code_line(block).strip()
    return s.startswith("constexpr std::string_view ") and "R\"GLSL(" in block


def global_decl(block: str):
    line = first_code_line(block)
    m = GLOBAL_RE.match(line)
    if not m:
        return None
    return m.group(1).strip(), m.group(2)


def remove_static(block: str) -> str:
    # Only touch a top-level leading static, preserving indentation and static locals inside bodies.
    return re.sub(r"(^\s{4})static\s+", r"\1", block, count=1, flags=re.M)


def function_signature(block: str) -> str | None:
    # Find the first top-level function opening brace while ignoring strings/comments sufficiently for signatures.
    text = block
    brace = text.find("{")
    semi = text.find(";")
    if brace < 0 or (semi >= 0 and semi < brace):
        return None
    prefix = text[:brace].rstrip()
    if "(" not in prefix or ")" not in prefix:
        return None
    prefix = re.sub(r"(^\s{4})static\s+", r"\1", prefix, count=1, flags=re.M)
    return strip_default_args(prefix) + ";\n"


def strip_default_args(signature: str) -> str:
    # Keep defaults in the declaration, not the definition. This helper is used on definitions below; declarations keep them.
    return signature


def strip_definition_defaults(block: str) -> str:
    # Remove default arguments only from the function signature before its opening body brace.
    brace = block.find("{")
    if brace < 0:
        return block
    sig, rest = block[:brace], block[brace:]
    out = []
    i = 0
    paren = sig.find("(")
    close = sig.rfind(")")
    if paren < 0 or close < paren:
        return block
    head, params, tail = sig[:paren + 1], sig[paren + 1:close], sig[close:]
    outp = []
    depth = 0
    skip = False
    quote = None
    j = 0
    while j < len(params):
        c = params[j]
        if quote:
            if c == "\\":
                if not skip: outp.append(c)
                if j + 1 < len(params) and not skip: outp.append(params[j + 1])
                j += 2; continue
            if c == quote: quote = None
            if not skip: outp.append(c)
            j += 1; continue
        if c in "\"'":
            quote = c
            if not skip: outp.append(c)
            j += 1; continue
        if c in "(<[{":
            depth += 1
            if not skip: outp.append(c)
        elif c in ")>]}":
            depth = max(depth - 1, 0)
            if not skip: outp.append(c)
        elif c == "=" and depth == 0:
            skip = True
        elif c == "," and depth == 0:
            skip = False
            outp.append(c)
        elif not skip:
            outp.append(c)
        j += 1
    return head + "".join(outp).rstrip() + tail + rest


def shader_extern(block: str) -> tuple[str, str] | None:
    s = first_code_line(block).strip()
    m = re.match(r"constexpr\s+std::string_view\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", s)
    if not m: return None
    name = m.group(1)
    definition = re.sub(r"(^\s{4})constexpr\s+std::string_view", r"\1const std::string_view", block, count=1, flags=re.M)
    return f"    extern const std::string_view {name};\n", definition


all_blocks: dict[str, list[str]] = {name: blocks_for(INC / name) for name in fragments}

types: list[str] = []
templates: list[str] = []
function_decls: list[str] = []
global_decls: list[str] = []
module_blocks: dict[str, list[str]] = {
    "shader/ShaderLibrary.cpp": [],
    "input/Input.cpp": [],
    "shader/Shader.cpp": [],
    "usb/USB.cpp": [],
    "audio/Audio.cpp": [],
    "media/Media.cpp": [],
    "runtime/RuntimeNative.cpp": [],
    "runtime/Runtime.cpp": [],
    "ui/RuntimeUI.cpp": [],
    "ui/UI.cpp": [],
}

forward_types: list[tuple[str, str, str | None]] = []


def module_for(fragment: str, block: str) -> str:
    text = block
    if fragment.startswith("MainShaderSources"): return "shader/ShaderLibrary.cpp"
    if fragment == "MainInput.inc": return "input/Input.cpp"
    if fragment == "MainInput02.inc": return "shader/Shader.cpp"
    if fragment == "MainMedia.inc":
        if "class RawUSB" in text or "PacketBuffer" in text or "USBStatsSnapshot" in text or "makePacket" in text: return "usb/USB.cpp"
        if "Audio" in text or any(x in text for x in ["shellQuote", "readCommand", "commandExists", "enumerateAudioSources"]): return "audio/Audio.cpp"
        if any(x in text for x in ["ShaderTransition", "compileShaders", "applyShaderDiagnostics", "switchShader", "lerpColorLinear"]): return "shader/Shader.cpp"
        return "media/Media.cpp"
    if fragment in ("MainRuntimeModel.inc", "MainRuntimeNative.inc"):
        return "runtime/RuntimeNative.cpp"
    if fragment == "MainRuntimeModel02.inc": return "runtime/Runtime.cpp"
    if fragment.startswith("MainRuntime"): return "ui/RuntimeUI.cpp"
    if fragment == "MainUI.inc": return "ui/UI.cpp"
    return "runtime/Runtime.cpp"

for fragment in fragments:
    for block in all_blocks[fragment]:
        line = first_code_line(block)
        s = line.strip()
        if not s:
            continue
        # using aliases supplied by Common.hpp
        if fragment == "MainShaderSources.inc" and s.startswith("using quartz::"):
            continue
        cname = constexpr_name(block)
        if cname in COMMON_CONSTANTS:
            continue
        if is_shader_source_constant(block):
            decldef = shader_extern(block)
            assert decldef
            global_decls.append(decldef[0])
            module_blocks[module_for(fragment, block)].append(decldef[1])
            continue
        if is_type(block):
            types.append(block)
            m = TYPE_RE.match(line)
            if m: forward_types.append((m.group(1), m.group(2), None))
            m = ENUM_RE.match(line)
            if m: forward_types.append(("enum", m.group(1), m.group(2) or "int"))
            continue
        if is_template(block):
            templates.append(remove_static(block))
            continue
        if is_static_assert(block):
            types.append(block)
            continue
        if cname:
            # Small constexpr data belongs in the shared model and is made inline for ODR safety.
            types.append(re.sub(r"(^\s{4})constexpr\s+", r"\1inline constexpr ", block, count=1, flags=re.M))
            continue
        gd = global_decl(block)
        if gd and "(" not in line.split("=", 1)[0]:
            ty, name = gd
            # Strip initializer from declaration and preserve the complete definition in its implementation module.
            # Known globals have straightforward declarable types.
            if name == "ShaderPresets": global_decls.append("    extern std::vector<ShaderPreset> ShaderPresets;\n")
            elif name == "g_SettingsStatus": global_decls.append("    extern std::string g_SettingsStatus;\n")
            elif name == "g_ShaderMaterialValues": global_decls.append("    extern std::unordered_map<std::string, std::string> g_ShaderMaterialValues;\n")
            elif name == "ShaderEditorFont": global_decls.append("    extern ImFont* ShaderEditorFont;\n")
            elif name == "ShaderEditorFonts": global_decls.append("    extern std::array<ImFont*, 20> ShaderEditorFonts;\n")
            else:
                # Keep unrecognized internal state inline in the model to avoid silently inventing a wrong extern type.
                types.append(re.sub(r"(^\s{4})static\s+", r"\1inline ", block, count=1, flags=re.M))
                continue
            module_blocks[module_for(fragment, block)].append(remove_static(block))
            continue
        sig = function_signature(block)
        if sig:
            function_decls.append(sig)
            definition = strip_definition_defaults(remove_static(block))
            module_blocks[module_for(fragment, block)].append(definition)
            continue
        # Prefix comments/odd declarations stay with their source module.
        module_blocks[module_for(fragment, block)].append(block)

# Deduplicate forward declarations while preserving order.
seen = set(); fwd_lines = []
for kind, name, underlying in forward_types:
    if name in seen: continue
    seen.add(name)
    if kind == "enum": fwd_lines.append(f"    enum class {name} : {underlying};\n")
    else: fwd_lines.append(f"    {kind} {name};\n")

INCLUDE.mkdir(parents=True, exist_ok=True)
(INCLUDE / "Common.hpp").write_text(COMMON)
(INCLUDE / "Forward.hpp").write_text("#pragma once\n#include \"Common.hpp\"\n\nnamespace quartz::client\n{\n" + "".join(fwd_lines) + "}\n")
(INCLUDE / "Globals.hpp").write_text("#pragma once\n#include \"Forward.hpp\"\n\nnamespace quartz::client\n{\n" + "".join(global_decls) + "}\n")
(INCLUDE / "Functions.hpp").write_text("#pragma once\n#include \"Globals.hpp\"\n\nnamespace quartz::client\n{\n" + "".join(function_decls) + "}\n")
(INCLUDE / "Model.hpp").write_text("#pragma once\n#include \"Functions.hpp\"\n\nnamespace quartz::client\n{\n" + "".join(types) + "\n" + "".join(templates) + "}\n")

# Public semantic facades; implementation is already split by subsystem while the shared model is being untangled.
facades = {
    "input/Input.hpp": "Model.hpp",
    "shader/Shader.hpp": "Model.hpp",
    "usb/USB.hpp": "Model.hpp",
    "audio/Audio.hpp": "Model.hpp",
    "media/Media.hpp": "Model.hpp",
    "runtime/Runtime.hpp": "Model.hpp",
    "ui/UI.hpp": "Model.hpp",
}
for rel, target in facades.items():
    p = INCLUDE / rel; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"#pragma once\n#include \"quartz/client/{target}\"\n")

# Implementation translation units.
for rel, blocks in module_blocks.items():
    p = SRC / rel; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text('#include "quartz/client/Model.hpp"\n\nnamespace quartz::client\n{\n' + "".join(blocks) + '\n}\n')

# Dedicated third-party implementation TU: exactly one GLAD and stb implementation after Main.cpp stops owning them.
third = SRC / "platform" / "ThirdParty.cpp"; third.parent.mkdir(parents=True, exist_ok=True)
third.write_text(r'''#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#undef GLAD_GL_IMPLEMENTATION

#if __has_include(<stb_image.h>)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif
''')

# Application facade and old main body moved out of the entry point.
app_h = INCLUDE / "Application.hpp"
app_h.write_text(r'''#pragma once
namespace quartz::client
{
    class Application
    {
    public:
        int run(int argc, char* argv[]);
    };
}
''')

entry = (INC / "MainEntry.inc").read_text()
start = entry.find('{')
end = entry.rfind('}')
body = entry[start + 1:end]
app_cpp = SRC / "Application.cpp"
app_cpp.write_text('#include "quartz/client/Application.hpp"\n#include "quartz/client/Model.hpp"\n\nnamespace quartz::client\n{\nint Application::run(int argc, char* argv[])\n{' + body + '\n}\n}\n')

(SRC / "Main.cpp").write_text(r'''#include "quartz/client/Application.hpp"

int main(int argc, char* argv[])
{
    quartz::client::Application application;
    return application.run(argc, argv);
}
''')

print(f"Generated {len(types)} model blocks, {len(templates)} templates, {len(function_decls)} free function declarations")
for rel, blocks in module_blocks.items(): print(rel, len(blocks))
