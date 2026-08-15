from pathlib import Path
import re
import textwrap

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
INC = ROOT / "include/quartz/client"
LEGACY_UI = SRC / "main/MainUI.inc"

PAGES = [
    ("Visualizer", "visualizer", "VisualizerPage"),
    ("Spectrum", "spectrum", "SpectrumPage"),
    ("Device", "device", "DevicePage"),
    ("RE / Bindings", "bindings", "BindingsPage"),
    ("QRPC Inspector", "qrpc", "QRPCPage"),
    ("USB", "usb", "USBPage"),
    ("Input Analyzer", "input", "InputPage"),
    ("RGB Profiler", "rgb", "RGBPage"),
    ("Audio Lab", "audio", "AudioPage"),
    ("Timeline", "timeline", "TimelinePage"),
    ("Firmware", "firmware", "FirmwarePage"),
    ("Performance", "performance", "PerformancePage"),
    ("Matrix timing", "matrix-timing", "MatrixTimingPage"),
]

page_dir = INC / "ui"
pages_include = page_dir / "pages"
pages_src = SRC / "ui/pages"
pages_include.mkdir(parents=True, exist_ok=True)
pages_src.mkdir(parents=True, exist_ok=True)

(page_dir / "Page.hpp").write_text(r'''#pragma once
#include <string_view>

namespace quartz::client::ui
{
    struct PageContext;
    class PageManager;

    enum class PagePresentation
    {
        Tab,
        Standalone
    };

    class Page
    {
    public:
        virtual ~Page() = default;
        [[nodiscard]] virtual std::string_view id() const noexcept = 0;
        [[nodiscard]] virtual std::string_view title() const noexcept = 0;
        [[nodiscard]] virtual PagePresentation presentation() const noexcept { return PagePresentation::Tab; }
        virtual void render(PageContext& context, PageManager& manager) = 0;
    };
}
''')

(page_dir / "PageContext.hpp").write_text(r'''#pragma once
#include "quartz/client/Model.hpp"

namespace quartz::client::ui
{
    struct PageContext
    {
        RawUSB& Usb;
        AudioSpectrum& Audio;
        MediaColorProvider& MediaColor;
        const EvdevKeyboard& KeyboardInput;
        ShaderFramebuffer& ShaderFramebuffer;
        ShaderTransitionState& ShaderTransition;
        ShaderEditorState& ShaderEditor;
        std::array<char, ShaderSourceCapacity>& VertexShaderSource;
        std::array<char, ShaderSourceCapacity>& FragmentShaderSource;
        std::array<char, ShaderPathCapacity>& VertexLoadPath;
        std::array<char, ShaderPathCapacity>& FragmentLoadPath;
        VisualizerSettings& Settings;
        const std::array<float, FFTSize>& AnalysisBands;
        const std::array<float, Columns>& MappedBands;
        const std::array<float, Columns>& SmoothedBands;
        const std::array<Color32, MatrixSize>& Framebuffer;
        SharedDeviceState& DeviceState;
        RuntimeBindingEngine& RuntimeBindings;
        RuntimeTelemetry& RuntimeTelemetryState;
        const AutoGainState& AutoGain;
        const AudioLevelSnapshot& AudioLevel;
        const ReactiveKeyState& ReactiveKeys;
        const RuntimeInputAnalytics& InputAnalytics;
        const RuntimeRGBAnalytics& RGBAnalytics;
        std::uint64_t SentFrames = 0;
        std::uint64_t DroppedFrames = 0;
        float AppCpuUsage = 0.0f;
        bool ScrollLockActive = false;
        bool CapsLockActive = false;
    };
}
''')

(page_dir / "PageManager.hpp").write_text(r'''#pragma once
#include "Page.hpp"
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace quartz::client::ui
{
    class PageManager
    {
    public:
        PageManager() = default;
        PageManager(PageManager&&) noexcept = default;
        PageManager& operator=(PageManager&&) noexcept = default;
        PageManager(const PageManager&) = delete;
        PageManager& operator=(const PageManager&) = delete;

        template<typename T, typename... Args>
        T& add(Args&&... args)
        {
            static_assert(std::is_base_of_v<Page, T>);
            auto page = std::make_unique<T>(std::forward<Args>(args)...);
            T& result = *page;
            _pages.emplace_back(std::move(page));
            return result;
        }

        [[nodiscard]] Page* find(std::string_view id) noexcept;
        [[nodiscard]] const Page* find(std::string_view id) const noexcept;
        [[nodiscard]] bool hasStandalonePage() const noexcept { return !_standaloneId.empty(); }
        [[nodiscard]] std::string_view standalonePageId() const noexcept { return _standaloneId; }

        bool open(std::string_view id);
        void closeStandalone() noexcept { _standaloneId.clear(); }
        void render(PageContext& context);

    private:
        std::vector<std::unique_ptr<Page>> _pages;
        std::string _standaloneId;
        std::string _requestedTabId;
    };

    [[nodiscard]] PageManager createDefaultPageManager();
}
''')

legacy = LEGACY_UI.read_text()
legacy_lines = legacy.splitlines(keepends=True)

def extract_tab(title: str) -> str:
    needle = f'if (ImGui::BeginTabItem("{title}"))'
    start = next((i for i, line in enumerate(legacy_lines) if needle in line), None)
    if start is None:
        raise RuntimeError(f"tab not found: {title}")
    # The legacy tab if lives at 12 spaces; its matching brace is the next brace at the same indentation.
    end = None
    for i in range(start + 2, len(legacy_lines)):
        if legacy_lines[i] == "            }\n" or legacy_lines[i] == "            }":
            end = i
            break
    if end is None:
        raise RuntimeError(f"tab end not found: {title}")
    body = "".join(legacy_lines[start + 2:end])
    # PageManager owns BeginTabItem/EndTabItem now.
    body = re.sub(r"^\s*ImGui::EndTabItem\(\);\s*$", "", body, flags=re.M)
    return textwrap.dedent(body).strip("\n") + "\n"

alias_map = [
    ("usb", "auto& usb = context.Usb;"),
    ("audio", "auto& audio = context.Audio;"),
    ("mediaColor", "auto& mediaColor = context.MediaColor;"),
    ("keyboardInput", "const auto& keyboardInput = context.KeyboardInput;"),
    ("shaderFramebuffer", "auto& shaderFramebuffer = context.ShaderFramebuffer;"),
    ("shaderTransition", "auto& shaderTransition = context.ShaderTransition;"),
    ("shaderEditor", "auto& shaderEditor = context.ShaderEditor;"),
    ("vertexShaderSource", "auto& vertexShaderSource = context.VertexShaderSource;"),
    ("fragmentShaderSource", "auto& fragmentShaderSource = context.FragmentShaderSource;"),
    ("vertexLoadPath", "auto& vertexLoadPath = context.VertexLoadPath;"),
    ("fragmentLoadPath", "auto& fragmentLoadPath = context.FragmentLoadPath;"),
    ("settings", "auto& settings = context.Settings;"),
    ("analysisBands", "const auto& analysisBands = context.AnalysisBands;"),
    ("mappedBands", "const auto& mappedBands = context.MappedBands;"),
    ("smoothedBands", "const auto& smoothedBands = context.SmoothedBands;"),
    ("framebuffer", "const auto& framebuffer = context.Framebuffer;"),
    ("deviceState", "auto& deviceState = context.DeviceState;"),
    ("runtimeBindings", "auto& runtimeBindings = context.RuntimeBindings;"),
    ("runtimeTelemetry", "auto& runtimeTelemetry = context.RuntimeTelemetryState;"),
    ("autoGain", "const auto& autoGain = context.AutoGain;"),
    ("audioLevel", "const auto& audioLevel = context.AudioLevel;"),
    ("reactiveKeys", "const auto& reactiveKeys = context.ReactiveKeys;"),
    ("inputAnalytics", "const auto& inputAnalytics = context.InputAnalytics;"),
    ("rgbAnalytics", "const auto& rgbAnalytics = context.RGBAnalytics;"),
    ("sentFrames", "const auto sentFrames = context.SentFrames;"),
    ("droppedFrames", "const auto droppedFrames = context.DroppedFrames;"),
    ("appCpuUsage", "const auto appCpuUsage = context.AppCpuUsage;"),
    ("scrollLockActive", "const auto scrollLockActive = context.ScrollLockActive;"),
    ("capsLockActive", "const auto capsLockActive = context.CapsLockActive;"),
]

def page_prelude(body: str) -> str:
    lines = []
    for token, declaration in alias_map:
        if re.search(rf"\b{re.escape(token)}\b", body):
            lines.append(declaration)
    if re.search(r"\bdefaults\b", body):
        lines.append("static const VisualizerSettings defaults{};")
    if re.search(r"\bconnected\b", body):
        lines.append("const bool connected = usb.isConnected();")
    snapshot_tokens = ["performance", "timingProbe", "hasPerformance", "hasTimingProbe", "receivedPackets"]
    if any(re.search(rf"\b{x}\b", body) for x in snapshot_tokens):
        if not any("deviceState =" in x for x in lines):
            lines.append("auto& deviceState = context.DeviceState;")
        lines.extend([
            "PerformanceSnapshot performance{};",
            "MatrixTimingProbeResult<ActiveProbeRows> timingProbe{};",
            "bool hasPerformance = false;",
            "bool hasTimingProbe = false;",
            "std::uint64_t receivedPackets = 0;",
            "{",
            "    std::lock_guard lock(deviceState.Mutex);",
            "    performance = deviceState.Performance;",
            "    timingProbe = deviceState.TimingProbe;",
            "    hasPerformance = deviceState.HasPerformance;",
            "    hasTimingProbe = deviceState.HasTimingProbe;",
            "    receivedPackets = deviceState.ReceivedPackets;",
            "}",
        ])
    return "\n".join(lines)

for title, page_id, class_name in PAGES:
    header = f'''#pragma once\n#include "quartz/client/ui/Page.hpp"\n\nnamespace quartz::client::ui\n{{\n    class {class_name} final : public Page\n    {{\n    public:\n        [[nodiscard]] std::string_view id() const noexcept override {{ return "{page_id}"; }}\n        [[nodiscard]] std::string_view title() const noexcept override {{ return "{title}"; }}\n        void render(PageContext& context, PageManager& manager) override;\n    }};\n}}\n'''
    (pages_include / f"{class_name}.hpp").write_text(header)
    body = extract_tab(title)
    body = body.replace("page = ViewPage::ShaderEditor;", 'manager.open("shader-editor");')
    prelude = page_prelude(body)
    cpp = f'''#include "quartz/client/ui/pages/{class_name}.hpp"\n#include "quartz/client/ui/PageContext.hpp"\n#include "quartz/client/ui/PageManager.hpp"\n\nnamespace quartz::client::ui\n{{\n    void {class_name}::render(PageContext& context, PageManager& manager)\n    {{\n'''
    if prelude:
        cpp += textwrap.indent(prelude, "        ") + "\n\n"
    # Avoid unused manager warnings without obscuring the API on pages that do not navigate.
    if "manager." not in body:
        cpp += "        (void)manager;\n"
    cpp += textwrap.indent(body, "        ")
    cpp += "    }\n}\n"
    (pages_src / f"{class_name}.cpp").write_text(cpp)

# Standalone shader editor page keeps the existing editor implementation but participates in the same Page lifecycle.
shader_header = '''#pragma once\n#include "quartz/client/ui/Page.hpp"\n\nnamespace quartz::client::ui\n{\n    class ShaderEditorPage final : public Page\n    {\n    public:\n        [[nodiscard]] std::string_view id() const noexcept override { return "shader-editor"; }\n        [[nodiscard]] std::string_view title() const noexcept override { return "Shader Editor"; }\n        [[nodiscard]] PagePresentation presentation() const noexcept override { return PagePresentation::Standalone; }\n        void render(PageContext& context, PageManager& manager) override;\n    };\n}\n'''
(pages_include / "ShaderEditorPage.hpp").write_text(shader_header)
(pages_src / "ShaderEditorPage.cpp").write_text(r'''#include "quartz/client/ui/pages/ShaderEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ShaderEditorPage::render(PageContext& context, PageManager& manager)
    {
        ViewPage legacyPage = ViewPage::ShaderEditor;
        drawShaderEditorPage(context.Usb, context.DeviceState, context.KeyboardInput, context.ShaderFramebuffer, context.ShaderTransition, context.ShaderEditor, legacyPage, context.VertexShaderSource, context.FragmentShaderSource, context.VertexLoadPath, context.FragmentLoadPath, context.Settings, context.Framebuffer, context.AppCpuUsage, context.ScrollLockActive, context.CapsLockActive);
        if (legacyPage == ViewPage::Main) manager.closeStandalone();
    }
}
''')

includes = "\n".join(f'#include "quartz/client/ui/pages/{class_name}.hpp"' for _, _, class_name in PAGES)
includes += '\n#include "quartz/client/ui/pages/ShaderEditorPage.hpp"\n'
adds = "\n".join(f"        manager.add<{class_name}>();" for _, _, class_name in PAGES)
manager_cpp = f'''#include "quartz/client/ui/PageManager.hpp"\n#include "quartz/client/ui/PageContext.hpp"\n{includes}\n#include <imgui.h>\n\nnamespace quartz::client::ui\n{{\n    Page* PageManager::find(const std::string_view id) noexcept\n    {{\n        for (const auto& page : _pages) if (page->id() == id) return page.get();\n        return nullptr;\n    }}\n\n    const Page* PageManager::find(const std::string_view id) const noexcept\n    {{\n        for (const auto& page : _pages) if (page->id() == id) return page.get();\n        return nullptr;\n    }}\n\n    bool PageManager::open(const std::string_view id)\n    {{\n        Page* page = find(id);\n        if (!page) return false;\n        if (page->presentation() == PagePresentation::Standalone) _standaloneId.assign(id);\n        else _requestedTabId.assign(id);\n        return true;\n    }}\n\n    void PageManager::render(PageContext& context)\n    {{\n        if (!_standaloneId.empty())\n        {{\n            Page* page = find(_standaloneId);\n            if (!page || page->presentation() != PagePresentation::Standalone) {{ _standaloneId.clear(); return; }}\n            page->render(context, *this);\n            return;\n        }}\n\n        if (!ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown | ImGuiTabBarFlags_TabListPopupButton)) return;\n        for (const auto& page : _pages)\n        {{\n            if (page->presentation() != PagePresentation::Tab) continue;\n            const ImGuiTabItemFlags flags = _requestedTabId == page->id() ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;\n            if (ImGui::BeginTabItem(page->title().data(), nullptr, flags))\n            {{\n                page->render(context, *this);\n                ImGui::EndTabItem();\n            }}\n        }}\n        _requestedTabId.clear();\n        ImGui::EndTabBar();\n    }}\n\n    PageManager createDefaultPageManager()\n    {{\n        PageManager manager;\n{adds}\n        manager.add<ShaderEditorPage>();\n        return manager;\n    }}\n}}\n'''
(SRC / "ui/PageManager.cpp").write_text(manager_cpp)

# Forward declaration is sufficient for the public drawUi declaration.
forward = INC / "Forward.hpp"
text = forward.read_text()
if "namespace ui" not in text:
    text = text.replace("namespace quartz::client\n{\n", "namespace quartz::client\n{\n    namespace ui { class PageManager; }\n", 1)
forward.write_text(text)

functions = INC / "Functions.hpp"
text = functions.read_text()
old = re.search(r"    void drawUi\([^\n]+\);", text)
if not old:
    raise RuntimeError("drawUi declaration not found")
new = old.group(0).replace("ViewPage& page", "ui::PageManager& pageManager")
text = text[:old.start()] + new + text[old.end():]
functions.write_text(text)

# Application owns the manager for the full UI lifetime.
application = SRC / "Application.cpp"
text = application.read_text()
if '#include "quartz/client/ui/PageManager.hpp"' not in text:
    text = text.replace('#include "quartz/client/Model.hpp"\n', '#include "quartz/client/Model.hpp"\n#include "quartz/client/ui/PageManager.hpp"\n')
text = text.replace("    ViewPage page = ViewPage::Main;", "    ui::PageManager pageManager = ui::createDefaultPageManager();")
text = text.replace("shaderEditor, page, vertexShaderSource", "shaderEditor, pageManager, vertexShaderSource")
application.write_text(text)

# Replace only the old drawUi dispatcher. All existing focused UI helpers stay in UI.cpp and are called by pages.
ui_cpp = SRC / "ui/UI.cpp"
text = ui_cpp.read_text()
start = text.find("    void drawUi(")
if start < 0:
    raise RuntimeError("drawUi definition not found")
namespace_end = text.rfind("\n}\n")
if namespace_end < start:
    raise RuntimeError("UI namespace end not found")

legacy_draw = legacy[legacy.find("    static void drawUi("):]
shell_start = legacy_draw.find("        static const VisualizerSettings defaults{};")
tab_start = legacy_draw.find('        if (ImGui::BeginTabBar("MainTabs"', shell_start)
if shell_start < 0 or tab_start < 0:
    raise RuntimeError("legacy UI shell could not be extracted")
shell = legacy_draw[shell_start:tab_start]

new_draw = r'''    void drawUi(RawUSB& usb, AudioSpectrum& audio, MediaColorProvider& mediaColor, const EvdevKeyboard& keyboardInput, ShaderFramebuffer& shaderFramebuffer, ShaderTransitionState& shaderTransition, ShaderEditorState& shaderEditor, ui::PageManager& pageManager, std::array<char, ShaderSourceCapacity>& vertexShaderSource, std::array<char, ShaderSourceCapacity>& fragmentShaderSource, std::array<char, ShaderPathCapacity>& vertexLoadPath, std::array<char, ShaderPathCapacity>& fragmentLoadPath, VisualizerSettings& settings, const std::array<float, FFTSize>& analysisBands, const std::array<float, Columns>& mappedBands, const std::array<float, Columns>& smoothedBands, const std::array<Color32, MatrixSize>& framebuffer, SharedDeviceState& deviceState, RuntimeBindingEngine& runtimeBindings, RuntimeTelemetry& runtimeTelemetry, const AutoGainState& autoGain, const AudioLevelSnapshot& audioLevel, const ReactiveKeyState& reactiveKeys, const RuntimeInputAnalytics& inputAnalytics, const RuntimeRGBAnalytics& rgbAnalytics, std::uint64_t sentFrames, std::uint64_t droppedFrames, const float appCpuUsage, const bool scrollLockActive, const bool capsLockActive)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Quartz K552X Visualizer", nullptr, windowFlags);
        drawPermanentHeader(usb);
        ImGui::BeginChild("MainScrollableBody", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);

        ui::PageContext context{usb, audio, mediaColor, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, analysisBands, mappedBands, smoothedBands, framebuffer, deviceState, runtimeBindings, runtimeTelemetry, autoGain, audioLevel, reactiveKeys, inputAnalytics, rgbAnalytics, sentFrames, droppedFrames, appCpuUsage, scrollLockActive, capsLockActive};
        if (pageManager.hasStandalonePage())
        {
            pageManager.render(context);
            ImGui::EndChild();
            ImGui::End();
            return;
        }
'''
new_draw += shell
new_draw += r'''        pageManager.render(context);
        ImGui::EndChild();
        ImGui::End();
    }
'''
text = text[:start] + new_draw + text[namespace_end:]
if '#include "quartz/client/ui/PageManager.hpp"' not in text:
    text = text.replace('#include "quartz/client/Model.hpp"\n', '#include "quartz/client/Model.hpp"\n#include "quartz/client/ui/PageManager.hpp"\n#include "quartz/client/ui/PageContext.hpp"\n')
ui_cpp.write_text(text)

print(f"generated PageManager and {len(PAGES) + 1} concrete Page subclasses")
