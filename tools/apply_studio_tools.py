from pathlib import Path


def write(path: str, content: str):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)


def replace_once(path: str, old: str, new: str):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise RuntimeError(f"missing replacement in {path}: {old[:160]!r}")
    p.write_text(text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# Shader workspace / external files / hot reload
# ---------------------------------------------------------------------------
write("include/quartz/client/shader/ShaderWorkspace.hpp", r'''#pragma once
#include "quartz/client/shader/ShaderEditor.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"

namespace quartz::client
{
    bool loadExternalShaderFile(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const std::filesystem::path& path, bool fragment);
    void clearExternalShaderFile(ShaderEditorState& editor, bool fragment) noexcept;
    bool pollExternalShaderHotReload(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, double now);
}
''')

write("src/shader/ShaderWorkspace.cpp", r'''#include "quartz/client/shader/ShaderWorkspace.hpp"
#include "quartz/client/Model.hpp"

namespace quartz::client
{
    namespace
    {
        bool loadExternal(const std::filesystem::path& path, std::array<char, ShaderSourceCapacity>& destination, std::filesystem::file_time_type& writeTime, std::string& error)
        {
            if (path.empty()) { error = "no external shader file selected"; return false; }
            std::array<char, ShaderSourceCapacity> loaded{};
            if (!loadTextFile(path, loaded)) { error = "failed to read " + path.string(); return false; }
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            if (ec) { error = "loaded shader but could not stat " + path.string() + ": " + ec.message(); return false; }
            destination = loaded;
            writeTime = time;
            error.clear();
            return true;
        }

        void syncEditors(ShaderEditorState& editor, const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
        {
            if (!editor.Initialized) initializeShaderEditors(editor, vertexSource.data(), fragmentSource.data());
            editor.Vertex.SetText(vertexSource.data());
            editor.Fragment.SetText(fragmentSource.data());
        }
    }

    bool loadExternalShaderFile(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const std::filesystem::path& path, const bool fragment)
    {
        std::string error;
        auto& destination = fragment ? fragmentSource : vertexSource;
        auto& writeTime = fragment ? editor.ExternalFragmentWriteTime : editor.ExternalVertexWriteTime;
        if (!loadExternal(path, destination, writeTime, error)) { editor.ExternalStatus = std::move(error); return false; }
        if (fragment) editor.ExternalFragmentPath = path; else editor.ExternalVertexPath = path;
        settings.ShaderPresetIndex = 0;
        settings.ShaderId.clear();
        syncEditors(editor, vertexSource, fragmentSource);
        const bool compiled = compileShaders(framebuffer, editor, vertexSource, fragmentSource);
        updateShaderDiagnostics(editor, framebuffer.status());
        editor.ExternalStatus = std::string(compiled ? "Loaded external " : "Loaded external file; compile failed: ") + path.string();
        return compiled;
    }

    void clearExternalShaderFile(ShaderEditorState& editor, const bool fragment) noexcept
    {
        if (fragment) { editor.ExternalFragmentPath.clear(); editor.ExternalFragmentWriteTime = {}; }
        else { editor.ExternalVertexPath.clear(); editor.ExternalVertexWriteTime = {}; }
        if (editor.ExternalFragmentPath.empty() && editor.ExternalVertexPath.empty()) editor.HotReloadExternal = false;
    }

    bool pollExternalShaderHotReload(ShaderEditorState& editor, ShaderFramebuffer& framebuffer, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const double now)
    {
        if (!editor.HotReloadExternal || now < editor.NextExternalPoll) return false;
        editor.NextExternalPoll = now + 0.20;
        bool changed = false;
        std::string changedFiles;
        auto poll = [&](const std::filesystem::path& path, std::filesystem::file_time_type& knownTime, std::array<char, ShaderSourceCapacity>& destination)
        {
            if (path.empty()) return;
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            if (ec) { editor.ExternalStatus = "hot reload stat failed for " + path.string() + ": " + ec.message(); return; }
            if (knownTime != std::filesystem::file_time_type{} && time == knownTime) return;
            std::array<char, ShaderSourceCapacity> loaded{};
            if (!loadTextFile(path, loaded)) { editor.ExternalStatus = "hot reload read failed for " + path.string(); return; }
            destination = loaded;
            knownTime = time;
            changed = true;
            if (!changedFiles.empty()) changedFiles += ", ";
            changedFiles += path.filename().string();
        };
        poll(editor.ExternalVertexPath, editor.ExternalVertexWriteTime, vertexSource);
        poll(editor.ExternalFragmentPath, editor.ExternalFragmentWriteTime, fragmentSource);
        if (!changed) return false;
        settings.ShaderPresetIndex = 0;
        settings.ShaderId.clear();
        syncEditors(editor, vertexSource, fragmentSource);
        const bool compiled = compileShaders(framebuffer, editor, vertexSource, fragmentSource);
        updateShaderDiagnostics(editor, framebuffer.status());
        editor.ExternalStatus = std::string(compiled ? "Hot reloaded " : "Hot reload compile failed for ") + changedFiles;
        return true;
    }
}
''')

replace_once("include/quartz/client/shader/ShaderEditor.hpp", "        bool ZoomResetWasDown = false;\n", "        bool ZoomResetWasDown = false;\n        std::filesystem::path ExternalVertexPath;\n        std::filesystem::path ExternalFragmentPath;\n        std::filesystem::file_time_type ExternalVertexWriteTime{};\n        std::filesystem::file_time_type ExternalFragmentWriteTime{};\n        bool HotReloadExternal = false;\n        double NextExternalPoll = 0.0;\n        std::string ExternalStatus;\n")

write("include/quartz/client/ui/pages/ShadersPage.hpp", r'''#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class ShadersPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "shaders"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Shaders"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Visual; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
''')

write("src/ui/pages/ShadersPage.cpp", r'''#include "quartz/client/ui/pages/ShadersPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/shader/ShaderWorkspace.hpp"

namespace quartz::client::ui
{
    namespace
    {
        std::optional<std::filesystem::path> pickShaderFile()
        {
            std::string result;
            if (commandExists("kdialog")) result = trim(bytesToString(readCommand("kdialog --getopenfilename . '*.frag *.vert *.glsl|GLSL shaders'")));
            else if (commandExists("zenity")) result = trim(bytesToString(readCommand("zenity --file-selection --file-filter='GLSL shaders | *.frag *.vert *.glsl'")));
            if (result.empty()) return std::nullopt;
            return std::filesystem::path(result);
        }

        void setPath(std::array<char, ShaderPathCapacity>& buffer, const std::filesystem::path& path) { std::snprintf(buffer.data(), buffer.size(), "%s", path.string().c_str()); }
    }

    void ShadersPage::render(PageContext& context, PageManager& manager)
    {
        auto& framebuffer = context.shaderFramebuffer;
        auto& transition = context.shaderTransition;
        auto& editor = context.shaderEditor;
        auto& vertexSource = context.vertexShaderSource;
        auto& fragmentSource = context.fragmentShaderSource;
        auto& vertexPath = context.vertexLoadPath;
        auto& fragmentPath = context.fragmentLoadPath;
        auto& settings = context.settings;
        static const VisualizerSettings defaults{};

        ImGui::TextWrapped("Shader catalog, external source files, hot reload, compilation and reflected material parameters live here. Opening a file keeps Quartz bound to it; importing copies a fragment shader into the Quartz catalog.");
        ImGui::SeparatorText("Current shader");
        const bool presetValid = settings.ShaderPresetIndex > 0 && settings.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size());
        const char* presetPreview = presetValid ? ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str() : "Custom / external";
        if (ImGui::BeginCombo("Catalog", presetPreview))
        {
            if (ImGui::Selectable("Custom / external", settings.ShaderPresetIndex == 0)) { settings.ShaderPresetIndex = 0; settings.ShaderId.clear(); }
            for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            {
                const bool selected = settings.ShaderPresetIndex == static_cast<int>(i + 1);
                std::string label = ShaderPresets[i].Name;
                if (!ShaderPresets[i].BuiltIn) label += "  [catalog]";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    clearExternalShaderFile(editor, true);
                    switchShaderPreset(framebuffer, transition, editor, vertexSource, fragmentSource, settings, static_cast<int>(i + 1), glfwGetTime(), settings.ShaderTransitionSeconds);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); if (ImGui::Button("Refresh catalog")) refreshShaderLibrary();
        ImGui::SameLine(); if (ImGui::Button("Edit..."))
        {
            initializeShaderEditors(editor, vertexSource.data(), fragmentSource.data());
            editor.Vertex.SetText(vertexSource.data()); editor.Fragment.SetText(fragmentSource.data()); updateShaderDiagnostics(editor, framebuffer.status()); manager.open("shader-editor");
        }
        if (ImGui::Button("Compile current")) compileShaders(framebuffer, editor, vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button("Save as Quartz defaults")) saveShaderSources(vertexSource, fragmentSource);
        ImGui::SameLine(); if (ImGui::Button("Restore default"))
        {
            clearExternalShaderFile(editor, false); clearExternalShaderFile(editor, true);
            settings.ShaderPresetIndex = 1; settings.ShaderId = ShaderPresets.front().Id;
            setShaderSource(vertexSource, DefaultVertexShaderSource); setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
            editor.Vertex.SetText(vertexSource.data()); editor.Fragment.SetText(fragmentSource.data()); compileShaders(framebuffer, editor, vertexSource, fragmentSource); saveShaderSources(vertexSource, fragmentSource);
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", framebuffer.status().c_str());

        ImGui::SeparatorText("External files");
        ImGui::SetNextItemWidth(-180.0f); ImGui::InputText("Vertex file", vertexPath.data(), vertexPath.size()); ImGui::SameLine();
        if (ImGui::Button("Select##vertex")) if (const auto path = pickShaderFile()) setPath(vertexPath, *path); ImGui::SameLine();
        if (ImGui::Button("Open##vertex")) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, vertexPath.data(), false);
        if (!editor.ExternalVertexPath.empty()) { ImGui::TextDisabled("bound vertex: %s", editor.ExternalVertexPath.string().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Unbind##vertex")) clearExternalShaderFile(editor, false); }

        ImGui::SetNextItemWidth(-280.0f); ImGui::InputText("Fragment file", fragmentPath.data(), fragmentPath.size()); ImGui::SameLine();
        if (ImGui::Button("Select##fragment")) if (const auto path = pickShaderFile()) setPath(fragmentPath, *path); ImGui::SameLine();
        if (ImGui::Button("Open##fragment")) loadExternalShaderFile(editor, framebuffer, vertexSource, fragmentSource, settings, fragmentPath.data(), true); ImGui::SameLine();
        if (ImGui::Button("Import to catalog"))
        {
            std::string importedId, error;
            if (importShaderToLibrary(fragmentPath.data(), importedId, error))
            {
                refreshShaderLibrary();
                if (switchShaderId(framebuffer, transition, editor, vertexSource, fragmentSource, settings, importedId, glfwGetTime(), settings.ShaderTransitionSeconds)) editor.ExternalStatus = "Imported to catalog as " + importedId;
                else editor.ExternalStatus = "Imported, but could not activate " + importedId;
            }
            else editor.ExternalStatus = std::move(error);
        }
        if (!editor.ExternalFragmentPath.empty()) { ImGui::TextDisabled("bound fragment: %s", editor.ExternalFragmentPath.string().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("Unbind##fragment")) clearExternalShaderFile(editor, true); }
        const bool hasExternal = !editor.ExternalVertexPath.empty() || !editor.ExternalFragmentPath.empty();
        if (!hasExternal) ImGui::BeginDisabled();
        ImGui::Checkbox("Hot reload external changes", &editor.HotReloadExternal);
        if (!hasExternal) ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("200 ms debounce/poll; external edits compile automatically");
        if (!editor.ExternalStatus.empty()) ImGui::TextWrapped("%s", editor.ExternalStatus.c_str());

        ImGui::SeparatorText("Render surface");
        int requestedSize[2]{settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight};
        if (ImGui::InputInt2("Framebuffer size", requestedSize))
        {
            settings.ShaderFramebufferWidth = std::clamp(requestedSize[0], static_cast<int>(Columns), MaxShaderDimension);
            settings.ShaderFramebufferHeight = std::clamp(requestedSize[1], static_cast<int>(Rows), MaxShaderDimension);
        }
        ImGui::SameLine(); if (ImGui::Button("Regenerate")) framebuffer.regenerate(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
        const char* downsampleModes[] = {"Average logical cell", "Average center 4x4", "Center pixel / exact"};
        ImGui::Combo("Downsample", &settings.ShaderDownsampleMode, downsampleModes, 3);
        ImGui::SliderFloat("Crossfade", &settings.ShaderTransitionSeconds, 0.0f, 5.0f, "%.2f s");
        ImGui::Checkbox("Recompile on editor text change", &settings.ShaderRecompileOnChange);
        ImGui::Checkbox("Key-state uniforms", &settings.ShaderKeyStateUniforms);
        ImGui::Checkbox("Caps Lock fixed color", &settings.ShaderCapsLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##capsShaderColor", settings.ShaderCapsLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Scroll Lock fixed color", &settings.ShaderScrollLockColorEnabled); ImGui::SameLine(); ImGui::ColorEdit3("##scrollShaderColor", settings.ShaderScrollLockColor.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Active surface %dx%d -> 16x7 QRPC framebuffer", framebuffer.width(), framebuffer.height());

        ImGui::SeparatorText("Material parameters");
        drawShaderMaterialEditor(framebuffer, 190.0f);
        ImGui::TextDisabled("Arbitrary active uniforms are reflected automatically; // @ui annotations can override labels/ranges/defaults.");
    }
}
''')

# Hot reload is polled from the main/render thread so OpenGL compilation stays on the owning context.
replace_once("src/Application.cpp", '#include "quartz/client/ui/ImGuiRuntime.hpp"\n', '#include "quartz/client/ui/ImGuiRuntime.hpp"\n#include "quartz/client/shader/ShaderWorkspace.hpp"\n')
replace_once("src/Application.cpp", "        const double currentFrame = glfwGetTime();\n        appCpuUsage = appCpuMeter.update(currentFrame);\n", "        const double currentFrame = glfwGetTime();\n        pollExternalShaderHotReload(shaderEditor, shaderFramebuffer, vertexShaderSource, fragmentShaderSource, settings, currentFrame);\n        appCpuUsage = appCpuMeter.update(currentFrame);\n")

# Visualizer becomes the live output page instead of a second settings warehouse.
write("src/ui/pages/VisualizerPage.cpp", r'''#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void VisualizerPage::render(PageContext& context, PageManager& manager)
    {
        auto& settings = context.settings;
        auto& usb = context.usb;
        static const VisualizerSettings defaults{};
        ImGui::TextWrapped("Live output. Detailed spectrum/audio tuning, shader management and RGB diagnostics live on their dedicated pages.");
        ImGui::SeparatorText("Output");
        ImGui::Checkbox("Enabled", &settings.Enabled); defaultButton("Enabled", settings.Enabled, defaults.Enabled);
        ImGui::Checkbox("Send framebuffer", &settings.SendFramebuffer); defaultButton("SendFramebuffer", settings.SendFramebuffer, defaults.SendFramebuffer);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderInt("Frame rate", &settings.FrameRate, 30, 500, "%d Hz"); defaultButton("FrameRate", settings.FrameRate, defaults.FrameRate);
        ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Global brightness", &settings.GlobalBrightness, 0.0f, 1.0f, "%.2f"); defaultButton("GlobalBrightness", settings.GlobalBrightness, defaults.GlobalBrightness);
        const char* modes[] = {"RGB wave", "Solid", "Shader"};
        ImGui::SetNextItemWidth(180.0f); ImGui::Combo("Output mode", &settings.BaseColorMode, modes, 3);
        if (settings.BaseColorMode == 0) { ImGui::SetNextItemWidth(180.0f); ImGui::SliderFloat("Wave speed", &settings.WaveSpeed, -2.0f, 2.0f, "%.3f"); }
        else if (settings.BaseColorMode == 1) ImGui::ColorEdit3("Solid color", settings.SolidColor.data());
        if (ImGui::Button("Shaders...")) manager.open("shaders"); ImGui::SameLine();
        if (ImGui::Button("Spectrum...")) manager.open("spectrum"); ImGui::SameLine();
        if (ImGui::Button("Audio...")) manager.open("audio"); ImGui::SameLine();
        if (ImGui::Button("RGB diagnostics...")) manager.open("rgb");

        ImGui::SeparatorText("Keyboard preview");
        drawFramebufferPreview(context.framebuffer, 0.72f, 720.0f, settings.LiveOutputInterpolation);
        ImGui::SliderFloat("Preview interpolation", &settings.LiveOutputInterpolation, 0.0f, 1.0f, "%.2f");
        ImGui::Text("Frames sent %llu   dropped %llu   USB %s", static_cast<unsigned long long>(context.sentFrames), static_cast<unsigned long long>(context.droppedFrames), usb.isConnected() ? "connected" : "disconnected");
        if (settings.BaseColorMode == 2)
        {
            const bool valid = settings.ShaderPresetIndex > 0 && settings.ShaderPresetIndex <= static_cast<int>(ShaderPresets.size());
            ImGui::Text("Shader: %s", valid ? ShaderPresets[static_cast<std::size_t>(settings.ShaderPresetIndex - 1)].Name.c_str() : (settings.ShaderId.empty() ? "Custom / external" : settings.ShaderId.c_str()));
            ImGui::SameLine(); ImGui::TextDisabled("%s", context.shaderFramebuffer.status().c_str());
        }
        if (ImGui::Button("Black out") && usb.isConnected()) { std::array<Color32, MatrixSize> black{}; sendFramebuffer(usb, black); }
    }
}
''')


# ---------------------------------------------------------------------------
# General memory scanner + pattern derivation
# ---------------------------------------------------------------------------
write("include/quartz/client/native/MemoryScanner.hpp", r'''#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <memory>

namespace quartz::client
{
    enum class MemoryScanValueType : int { U8, I8, U16, I16, U32, I32, U64, I64, Float, Double, Pointer, Bool, Utf8String, Utf16String, ByteArray };
    enum class MemoryScanComparison : int { Exact, NotEqual, UnknownInitial, Changed, Unchanged, Increased, Decreased, IncreasedBy, DecreasedBy, Greater, Less, Between, ChangedFromTo };

    struct MemoryScanRequest
    {
        pid_t Pid = 0;
        MemoryScanValueType Type = MemoryScanValueType::I32;
        MemoryScanComparison Comparison = MemoryScanComparison::Exact;
        std::string ValueA;
        std::string ValueB;
        bool WritableOnly = true;
        bool ExecutableOnly = false;
        bool Aligned = true;
        bool CaseSensitive = true;
    };

    struct MemoryScanStats
    {
        bool Running = false;
        std::uint64_t Bytes = 0;
        std::uint64_t TotalBytes = 0;
        std::uint64_t Candidates = 0;
        double Seconds = 0.0;
        double MiBs = 0.0;
        std::string Status;
    };

    struct MemoryScanResultRow { std::uintptr_t Address = 0; std::string Value; };
    struct MemoryScanSnapshot;
    struct MemoryScanJobState;

    class MemoryScanner
    {
    public:
        MemoryScanner() = default;
        ~MemoryScanner();
        MemoryScanner(const MemoryScanner&) = delete;
        MemoryScanner& operator=(const MemoryScanner&) = delete;

        bool newScan(const MemoryScanRequest& request, std::string& error);
        bool nextScan(const MemoryScanRequest& request, std::string& error);
        void cancel() noexcept;
        void poll();
        [[nodiscard]] bool running() const noexcept;
        [[nodiscard]] bool hasSnapshot() const noexcept;
        [[nodiscard]] MemoryScanStats stats() const;
        [[nodiscard]] std::vector<MemoryScanResultRow> results(std::size_t limit = 256) const;
        [[nodiscard]] pid_t pid() const noexcept;
        [[nodiscard]] MemoryScanValueType valueType() const noexcept;

    private:
        std::shared_ptr<MemoryScanJobState> _job;
        std::unique_ptr<MemoryScanSnapshot> _snapshot;
        MemoryScanStats _lastStats;
    };

    const char* memoryScanValueTypeName(MemoryScanValueType type) noexcept;
    const char* memoryScanComparisonName(MemoryScanComparison comparison) noexcept;
    std::string deriveRuntimeBytePattern(pid_t pid, std::uintptr_t start, std::uintptr_t end, bool wildcardRelocations, std::string& error);
}
''')

write("src/native/MemoryScanner.cpp", r'''#include "quartz/client/native/MemoryScanner.hpp"
#include "quartz/client/Model.hpp"
#include "quartz/client/async/ThreadPool.hpp"
#include <atomic>
#include <cstring>
#if QUARTZ_HAS_ZYDIS
#include <Zydis/Zydis.h>
#endif

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t ScanChunk = 4 * 1024 * 1024;

        struct MemoryScanRegionSnapshot
        {
            std::uintptr_t Base = 0;
            std::size_t ScanLength = 0;
            std::size_t FirstOffset = 0;
            std::size_t Step = 1;
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint64_t> Candidates;
            std::uint64_t CandidateCount = 0;
        };

        struct ParsedScanValue
        {
            std::size_t Width = 0;
            long double NumericA = 0.0L;
            long double NumericB = 0.0L;
            bool HasA = false;
            bool HasB = false;
            std::vector<std::uint8_t> A;
            std::vector<std::uint8_t> B;
            std::vector<std::uint8_t> MaskA;
            std::vector<std::uint8_t> MaskB;
        };

        std::size_t numericWidth(const MemoryScanValueType type) noexcept
        {
            switch (type)
            {
            case MemoryScanValueType::U8: case MemoryScanValueType::I8: case MemoryScanValueType::Bool: return 1;
            case MemoryScanValueType::U16: case MemoryScanValueType::I16: return 2;
            case MemoryScanValueType::U32: case MemoryScanValueType::I32: case MemoryScanValueType::Float: return 4;
            case MemoryScanValueType::U64: case MemoryScanValueType::I64: case MemoryScanValueType::Double: case MemoryScanValueType::Pointer: return 8;
            default: return 0;
            }
        }

        bool isNumeric(const MemoryScanValueType type) noexcept { return numericWidth(type) != 0; }
        bool comparisonNeedsPrevious(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Changed || c == MemoryScanComparison::Unchanged || c == MemoryScanComparison::Increased || c == MemoryScanComparison::Decreased || c == MemoryScanComparison::IncreasedBy || c == MemoryScanComparison::DecreasedBy || c == MemoryScanComparison::ChangedFromTo; }
        bool comparisonNeedsA(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Exact || c == MemoryScanComparison::NotEqual || c == MemoryScanComparison::IncreasedBy || c == MemoryScanComparison::DecreasedBy || c == MemoryScanComparison::Greater || c == MemoryScanComparison::Less || c == MemoryScanComparison::Between || c == MemoryScanComparison::ChangedFromTo; }
        bool comparisonNeedsB(const MemoryScanComparison c) noexcept { return c == MemoryScanComparison::Between || c == MemoryScanComparison::ChangedFromTo; }

        bool parseNumeric(const std::string& text, long double& value)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            errno = 0;
            value = std::strtold(text.c_str(), &end);
            return errno == 0 && end == text.c_str() + text.size() && std::isfinite(value);
        }

        std::vector<std::uint8_t> utf16Bytes(const std::string_view text)
        {
            std::vector<std::uint8_t> result;
            result.reserve(text.size() * 2);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
            const unsigned char* end = p + text.size();
            while (p < end)
            {
                std::uint32_t cp = *p++;
                if (cp >= 0xC2 && cp <= 0xDF && p < end) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
                else if (cp >= 0xE0 && cp <= 0xEF && p + 1 < end) { cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
                else if (cp >= 0xF0 && cp <= 0xF4 && p + 2 < end) { cp = ((cp & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
                if (cp <= 0xFFFF)
                {
                    result.push_back(static_cast<std::uint8_t>(cp & 0xFF)); result.push_back(static_cast<std::uint8_t>((cp >> 8) & 0xFF));
                }
                else
                {
                    cp -= 0x10000; const std::uint16_t high = static_cast<std::uint16_t>(0xD800 + (cp >> 10)), low = static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF));
                    result.push_back(static_cast<std::uint8_t>(high & 0xFF)); result.push_back(static_cast<std::uint8_t>(high >> 8)); result.push_back(static_cast<std::uint8_t>(low & 0xFF)); result.push_back(static_cast<std::uint8_t>(low >> 8));
                }
            }
            return result;
        }

        bool parseBytes(const MemoryScanValueType type, const std::string& text, std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& masks, std::string& error)
        {
            bytes.clear(); masks.clear();
            if (type == MemoryScanValueType::Utf8String)
            {
                bytes.assign(text.begin(), text.end()); masks.assign(bytes.size(), 0xFF); if (bytes.empty()) { error = "string cannot be empty"; return false; } return true;
            }
            if (type == MemoryScanValueType::Utf16String)
            {
                bytes = utf16Bytes(text); masks.assign(bytes.size(), 0xFF); if (bytes.empty()) { error = "string cannot be empty"; return false; } return true;
            }
            if (type == MemoryScanValueType::ByteArray) return parseRuntimeHexPattern(text, bytes, masks, error);
            return false;
        }

        bool parseRequestValues(const MemoryScanRequest& request, const std::size_t existingWidth, ParsedScanValue& parsed, std::string& error)
        {
            parsed = {};
            if (isNumeric(request.Type))
            {
                parsed.Width = numericWidth(request.Type);
                if (comparisonNeedsA(request.Comparison) && !parseNumeric(request.ValueA, parsed.NumericA)) { error = "invalid first numeric value"; return false; }
                if (comparisonNeedsB(request.Comparison) && !parseNumeric(request.ValueB, parsed.NumericB)) { error = "invalid second numeric value"; return false; }
                parsed.HasA = comparisonNeedsA(request.Comparison); parsed.HasB = comparisonNeedsB(request.Comparison);
                return true;
            }
            std::string localError;
            if (comparisonNeedsA(request.Comparison) || existingWidth == 0)
            {
                if (!parseBytes(request.Type, request.ValueA, parsed.A, parsed.MaskA, localError)) { error = localError.empty() ? "a value is required to determine scan width" : localError; return false; }
                parsed.HasA = true; parsed.Width = parsed.A.size();
            }
            else parsed.Width = existingWidth;
            if (comparisonNeedsB(request.Comparison))
            {
                if (!parseBytes(request.Type, request.ValueB, parsed.B, parsed.MaskB, localError)) { error = localError; return false; }
                if (parsed.B.size() != parsed.Width) { error = "from/to values must have the same byte length"; return false; }
                parsed.HasB = true;
            }
            if (existingWidth != 0 && parsed.Width != existingWidth) { error = "value width differs from the current scan"; return false; }
            if ((request.Comparison == MemoryScanComparison::Increased || request.Comparison == MemoryScanComparison::Decreased || request.Comparison == MemoryScanComparison::IncreasedBy || request.Comparison == MemoryScanComparison::DecreasedBy || request.Comparison == MemoryScanComparison::Greater || request.Comparison == MemoryScanComparison::Less || request.Comparison == MemoryScanComparison::Between) && !isNumeric(request.Type)) { error = "ordered comparisons require a numeric value type"; return false; }
            return parsed.Width != 0;
        }

        long double numericAt(const MemoryScanValueType type, const std::uint8_t* data)
        {
            switch (type)
            {
            case MemoryScanValueType::U8: { std::uint8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::I8: { std::int8_t v; std::memcpy(&v, data, 1); return v; }
            case MemoryScanValueType::U16: { std::uint16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::I16: { std::int16_t v; std::memcpy(&v, data, 2); return v; }
            case MemoryScanValueType::U32: { std::uint32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::I32: { std::int32_t v; std::memcpy(&v, data, 4); return v; }
            case MemoryScanValueType::U64: case MemoryScanValueType::Pointer: { std::uint64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::I64: { std::int64_t v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Float: { float v; std::memcpy(&v, data, 4); return static_cast<long double>(v); }
            case MemoryScanValueType::Double: { double v; std::memcpy(&v, data, 8); return static_cast<long double>(v); }
            case MemoryScanValueType::Bool: { std::uint8_t v; std::memcpy(&v, data, 1); return v != 0 ? 1.0L : 0.0L; }
            default: return 0.0L;
            }
        }

        bool bytesEqual(const std::uint8_t* value, const std::vector<std::uint8_t>& expected, const std::vector<std::uint8_t>& masks, const MemoryScanValueType type, const bool caseSensitive)
        {
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                std::uint8_t actual = value[i], wanted = expected[i];
                if (!caseSensitive && type == MemoryScanValueType::Utf8String) { actual = static_cast<std::uint8_t>(std::tolower(actual)); wanted = static_cast<std::uint8_t>(std::tolower(wanted)); }
                if (!caseSensitive && type == MemoryScanValueType::Utf16String && (i & 1) == 0 && value[i + 1] == 0 && expected[i + 1] == 0) { actual = static_cast<std::uint8_t>(std::tolower(actual)); wanted = static_cast<std::uint8_t>(std::tolower(wanted)); }
                const std::uint8_t mask = masks.empty() ? 0xFF : masks[i];
                if ((actual & mask) != (wanted & mask)) return false;
            }
            return true;
        }

        bool valuesEqual(const MemoryScanValueType type, const std::uint8_t* a, const std::uint8_t* b, const std::size_t width, const bool caseSensitive)
        {
            if (isNumeric(type)) return numericAt(type, a) == numericAt(type, b);
            if (caseSensitive || type == MemoryScanValueType::ByteArray) return std::memcmp(a, b, width) == 0;
            if (type == MemoryScanValueType::Utf8String) for (std::size_t i = 0; i < width; ++i) if (std::tolower(a[i]) != std::tolower(b[i])) return false; else { }
            else if (type == MemoryScanValueType::Utf16String) for (std::size_t i = 0; i + 1 < width; i += 2) { const std::uint16_t av = static_cast<std::uint16_t>(a[i] | (a[i + 1] << 8)), bv = static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8)); if (av < 128 && bv < 128 ? std::tolower(av) != std::tolower(bv) : av != bv) return false; }
            return true;
        }

        bool matches(const MemoryScanRequest& request, const ParsedScanValue& parsed, const std::uint8_t* current, const std::uint8_t* previous)
        {
            if (request.Comparison == MemoryScanComparison::UnknownInitial) return true;
            if (isNumeric(request.Type))
            {
                const long double now = numericAt(request.Type, current), before = previous ? numericAt(request.Type, previous) : 0.0L;
                switch (request.Comparison)
                {
                case MemoryScanComparison::Exact: return now == parsed.NumericA;
                case MemoryScanComparison::NotEqual: return now != parsed.NumericA;
                case MemoryScanComparison::Changed: return previous && now != before;
                case MemoryScanComparison::Unchanged: return previous && now == before;
                case MemoryScanComparison::Increased: return previous && now > before;
                case MemoryScanComparison::Decreased: return previous && now < before;
                case MemoryScanComparison::IncreasedBy: return previous && now == before + parsed.NumericA;
                case MemoryScanComparison::DecreasedBy: return previous && now == before - parsed.NumericA;
                case MemoryScanComparison::Greater: return now > parsed.NumericA;
                case MemoryScanComparison::Less: return now < parsed.NumericA;
                case MemoryScanComparison::Between: return now >= std::min(parsed.NumericA, parsed.NumericB) && now <= std::max(parsed.NumericA, parsed.NumericB);
                case MemoryScanComparison::ChangedFromTo: return previous && before == parsed.NumericA && now == parsed.NumericB;
                default: return true;
                }
            }
            switch (request.Comparison)
            {
            case MemoryScanComparison::Exact: return bytesEqual(current, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive);
            case MemoryScanComparison::NotEqual: return !bytesEqual(current, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive);
            case MemoryScanComparison::Changed: return previous && !valuesEqual(request.Type, current, previous, parsed.Width, request.CaseSensitive);
            case MemoryScanComparison::Unchanged: return previous && valuesEqual(request.Type, current, previous, parsed.Width, request.CaseSensitive);
            case MemoryScanComparison::ChangedFromTo: return previous && bytesEqual(previous, parsed.A, parsed.MaskA, request.Type, request.CaseSensitive) && bytesEqual(current, parsed.B, parsed.MaskB, request.Type, request.CaseSensitive);
            default: return false;
            }
        }

        bool bit(const std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { return (bits[index / 64] & (1ULL << (index & 63))) != 0; }
        void setBit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { bits[index / 64] |= 1ULL << (index & 63); }
        void clearBit(std::vector<std::uint64_t>& bits, const std::size_t index) noexcept { bits[index / 64] &= ~(1ULL << (index & 63)); }

        std::string formatValue(const MemoryScanValueType type, const std::uint8_t* data, const std::size_t width)
        {
            std::ostringstream out;
            if (isNumeric(type))
            {
                if (type == MemoryScanValueType::Pointer) { std::uint64_t v; std::memcpy(&v, data, 8); out << "0x" << std::hex << std::uppercase << v; }
                else if (type == MemoryScanValueType::Float || type == MemoryScanValueType::Double) out << std::setprecision(12) << static_cast<double>(numericAt(type, data));
                else out << std::fixed << std::setprecision(0) << numericAt(type, data);
                return out.str();
            }
            if (type == MemoryScanValueType::Utf8String) return std::string(reinterpret_cast<const char*>(data), width);
            if (type == MemoryScanValueType::Utf16String)
            {
                std::string result; result.reserve(width / 2);
                for (std::size_t i = 0; i + 1 < width; i += 2) { const std::uint16_t cp = static_cast<std::uint16_t>(data[i] | (data[i + 1] << 8)); result.push_back(cp >= 32 && cp < 127 ? static_cast<char>(cp) : '.'); }
                return result;
            }
            for (std::size_t i = 0; i < width; ++i) { if (i) out << ' '; out << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]); }
            return out.str();
        }
    }

    struct MemoryScanSnapshot
    {
        pid_t Pid = 0;
        MemoryScanValueType Type = MemoryScanValueType::I32;
        std::size_t Width = 0;
        bool Aligned = true;
        bool CaseSensitive = true;
        std::vector<MemoryScanRegionSnapshot> Regions;
        std::uint64_t Candidates = 0;
    };

    struct MemoryScanJobState
    {
        std::mutex Mutex;
        std::atomic_bool Finished{false};
        std::atomic_bool Success{false};
        std::atomic_bool Cancelled{false};
        std::atomic<std::uint64_t> Bytes{0};
        std::atomic<std::uint64_t> TotalBytes{0};
        std::atomic<std::uint64_t> Candidates{0};
        double Started = runtimeSteadySeconds();
        double Seconds = 0.0;
        std::string Status = "queued";
        std::unique_ptr<MemoryScanSnapshot> Result;
    };

    const char* memoryScanValueTypeName(const MemoryScanValueType type) noexcept
    {
        static constexpr const char* Names[] = {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "float", "double", "pointer", "bool", "UTF-8 string", "UTF-16 string", "byte array"};
        return Names[std::clamp(static_cast<int>(type), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    const char* memoryScanComparisonName(const MemoryScanComparison c) noexcept
    {
        static constexpr const char* Names[] = {"Exact value", "Not equal", "Unknown initial value", "Changed", "Unchanged", "Increased", "Decreased", "Increased by", "Decreased by", "Greater than", "Less than", "Between", "Changed from -> to"};
        return Names[std::clamp(static_cast<int>(c), 0, static_cast<int>(std::size(Names)) - 1)];
    }

    MemoryScanner::~MemoryScanner() { cancel(); _job.reset(); }
    bool MemoryScanner::running() const noexcept { return _job && !_job->Finished.load(std::memory_order_acquire); }
    bool MemoryScanner::hasSnapshot() const noexcept { return _snapshot != nullptr; }
    pid_t MemoryScanner::pid() const noexcept { return _snapshot ? _snapshot->Pid : 0; }
    MemoryScanValueType MemoryScanner::valueType() const noexcept { return _snapshot ? _snapshot->Type : MemoryScanValueType::I32; }
    void MemoryScanner::cancel() noexcept { if (_job) _job->Cancelled.store(true, std::memory_order_release); }

    MemoryScanStats MemoryScanner::stats() const
    {
        if (!_job) return _lastStats;
        MemoryScanStats stats;
        stats.Running = !_job->Finished.load(std::memory_order_acquire);
        stats.Bytes = _job->Bytes.load(std::memory_order_relaxed); stats.TotalBytes = _job->TotalBytes.load(std::memory_order_relaxed); stats.Candidates = _job->Candidates.load(std::memory_order_relaxed);
        stats.Seconds = stats.Running ? std::max(runtimeSteadySeconds() - _job->Started, 0.000001) : _job->Seconds;
        stats.MiBs = stats.Bytes / (1024.0 * 1024.0) / std::max(stats.Seconds, 0.000001);
        { std::lock_guard lock(_job->Mutex); stats.Status = _job->Status; }
        return stats;
    }

    void MemoryScanner::poll()
    {
        if (!_job || !_job->Finished.load(std::memory_order_acquire)) return;
        MemoryScanStats done = stats(); done.Running = false;
        {
            std::lock_guard lock(_job->Mutex);
            if (_job->Result) _snapshot = std::move(_job->Result);
            done.Status = _job->Status;
        }
        _lastStats = std::move(done); _job.reset();
    }

    bool MemoryScanner::newScan(const MemoryScanRequest& request, std::string& error)
    {
        if (running()) { error = "a scan is already running"; return false; }
        if (request.Pid <= 0) { error = "select a process first"; return false; }
        if (comparisonNeedsPrevious(request.Comparison)) { error = "this comparison requires Next Scan"; return false; }
        ParsedScanValue parsed;
        if (!parseRequestValues(request, 0, parsed, error)) return false;
        _snapshot.reset(); _lastStats = {};
        auto job = std::make_shared<MemoryScanJobState>(); _job = job;
        async::globalThreadPool().submit([job, request, parsed](std::stop_token stop)
        {
            auto snapshot = std::make_unique<MemoryScanSnapshot>(); snapshot->Pid = request.Pid; snapshot->Type = request.Type; snapshot->Width = parsed.Width; snapshot->Aligned = request.Aligned; snapshot->CaseSensitive = request.CaseSensitive;
            const auto regions = enumerateRuntimeRegions(request.Pid);
            std::uint64_t total = 0; for (const auto& r : regions) if (r.Readable && (!request.WritableOnly || r.Writable) && (!request.ExecutableOnly || r.Executable)) total += r.End - r.Base;
            job->TotalBytes.store(total, std::memory_order_relaxed);
            { std::lock_guard lock(job->Mutex); job->Status = "scanning process memory"; }
            std::string readError;
            for (const auto& region : regions)
            {
                if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                if (!region.Readable || (request.WritableOnly && !region.Writable) || (request.ExecutableOnly && !region.Executable)) continue;
                for (std::uintptr_t cursor = region.Base; cursor < region.End;)
                {
                    if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                    const std::size_t logical = static_cast<std::size_t>(std::min<std::uintptr_t>(ScanChunk, region.End - cursor));
                    const std::size_t readSize = static_cast<std::size_t>(std::min<std::uintptr_t>(logical + parsed.Width - 1, region.End - cursor));
                    std::vector<std::uint8_t> current(readSize);
                    if (readProcessMemoryBlock(request.Pid, cursor, current, readError))
                    {
                        MemoryScanRegionSnapshot part; part.Base = cursor; part.ScanLength = logical; part.Bytes = std::move(current); part.Step = request.Aligned ? parsed.Width : 1;
                        part.FirstOffset = request.Aligned ? (parsed.Width - (cursor % parsed.Width)) % parsed.Width : 0;
                        const std::size_t slots = part.FirstOffset < logical ? ((logical - 1 - part.FirstOffset) / part.Step + 1) : 0;
                        part.Candidates.assign((slots + 63) / 64, 0);
                        for (std::size_t i = 0; i < slots; ++i)
                        {
                            const std::size_t offset = part.FirstOffset + i * part.Step;
                            if (offset + parsed.Width > part.Bytes.size()) break;
                            if (matches(request, parsed, part.Bytes.data() + offset, nullptr)) { setBit(part.Candidates, i); ++part.CandidateCount; }
                        }
                        snapshot->Candidates += part.CandidateCount; snapshot->Regions.emplace_back(std::move(part));
                        job->Candidates.store(snapshot->Candidates, std::memory_order_relaxed);
                    }
                    cursor += logical; job->Bytes.fetch_add(logical, std::memory_order_relaxed);
                }
            }
            const bool cancelled = stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire);
            job->Seconds = std::max(runtimeSteadySeconds() - job->Started, 0.000001);
            { std::lock_guard lock(job->Mutex); job->Result = std::move(snapshot); job->Status = cancelled ? "scan cancelled" : "scan complete"; }
            job->Success.store(!cancelled, std::memory_order_release); job->Finished.store(true, std::memory_order_release);
        }, async::TaskPriority::Background);
        error.clear(); return true;
    }

    bool MemoryScanner::nextScan(const MemoryScanRequest& request, std::string& error)
    {
        poll();
        if (running()) { error = "a scan is already running"; return false; }
        if (!_snapshot) { error = "run New Scan first"; return false; }
        if (request.Pid != _snapshot->Pid) { error = "process changed; start a New Scan"; return false; }
        if (request.Type != _snapshot->Type) { error = "value type changed; start a New Scan"; return false; }
        if (request.Comparison == MemoryScanComparison::UnknownInitial) { error = "Unknown initial value is only valid for New Scan"; return false; }
        ParsedScanValue parsed;
        if (!parseRequestValues(request, _snapshot->Width, parsed, error)) return false;
        auto job = std::make_shared<MemoryScanJobState>(); job->Result = std::move(_snapshot); job->Candidates.store(job->Result->Candidates, std::memory_order_relaxed);
        std::uint64_t total = 0; for (const auto& part : job->Result->Regions) total += part.ScanLength; job->TotalBytes.store(total, std::memory_order_relaxed); _job = job;
        async::globalThreadPool().submit([job, request, parsed](std::stop_token stop)
        {
            { std::lock_guard lock(job->Mutex); job->Status = "filtering previous candidates"; }
            auto* snapshot = job->Result.get(); std::uint64_t remaining = snapshot->Candidates; std::string readError;
            for (auto& part : snapshot->Regions)
            {
                if (stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire)) break;
                std::vector<std::uint8_t> current(part.Bytes.size());
                if (!readProcessMemoryBlock(request.Pid, part.Base, current, readError))
                {
                    remaining -= part.CandidateCount; part.CandidateCount = 0; std::fill(part.Candidates.begin(), part.Candidates.end(), 0); job->Bytes.fetch_add(part.ScanLength, std::memory_order_relaxed); continue;
                }
                const std::size_t slots = part.Candidates.size() * 64;
                for (std::size_t i = 0; i < slots; ++i)
                {
                    if (!bit(part.Candidates, i)) continue;
                    const std::size_t offset = part.FirstOffset + i * part.Step;
                    if (offset + parsed.Width > part.Bytes.size() || offset + parsed.Width > current.size() || !matches(request, parsed, current.data() + offset, part.Bytes.data() + offset)) { clearBit(part.Candidates, i); --part.CandidateCount; --remaining; }
                }
                part.Bytes.swap(current); job->Candidates.store(remaining, std::memory_order_relaxed); job->Bytes.fetch_add(part.ScanLength, std::memory_order_relaxed);
            }
            snapshot->Candidates = remaining;
            const bool cancelled = stop.stop_requested() || job->Cancelled.load(std::memory_order_acquire);
            job->Seconds = std::max(runtimeSteadySeconds() - job->Started, 0.000001);
            { std::lock_guard lock(job->Mutex); job->Status = cancelled ? "next scan cancelled" : "next scan complete"; }
            job->Success.store(!cancelled, std::memory_order_release); job->Finished.store(true, std::memory_order_release);
        }, async::TaskPriority::Background);
        error.clear(); return true;
    }

    std::vector<MemoryScanResultRow> MemoryScanner::results(const std::size_t limit) const
    {
        std::vector<MemoryScanResultRow> rows;
        if (!_snapshot || limit == 0) return rows;
        rows.reserve(std::min<std::uint64_t>(_snapshot->Candidates, limit));
        for (const auto& part : _snapshot->Regions)
        {
            const std::size_t slots = part.Candidates.size() * 64;
            for (std::size_t i = 0; i < slots && rows.size() < limit; ++i)
            {
                if (!bit(part.Candidates, i)) continue;
                const std::size_t offset = part.FirstOffset + i * part.Step;
                if (offset + _snapshot->Width > part.Bytes.size()) continue;
                rows.push_back({part.Base + offset, formatValue(_snapshot->Type, part.Bytes.data() + offset, _snapshot->Width)});
            }
            if (rows.size() >= limit) break;
        }
        return rows;
    }

    std::string deriveRuntimeBytePattern(const pid_t pid, const std::uintptr_t start, const std::uintptr_t end, const bool wildcardRelocations, std::string& error)
    {
        if (pid <= 0 || start == 0 || end <= start) { error = "enter a valid PID and address range"; return {}; }
        const std::size_t size = static_cast<std::size_t>(end - start);
        if (size > 4096) { error = "pattern derivation is limited to 4096 bytes"; return {}; }
        std::vector<std::uint8_t> bytes(size); if (!readProcessMemoryBlock(pid, start, bytes, error)) return {};
        std::vector<bool> wildcard(size, false);
#if QUARTZ_HAS_ZYDIS
        if (wildcardRelocations)
        {
            ZydisDecoder decoder{}; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                ZydisDecodedInstruction instruction{}; ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + offset, bytes.size() - offset, &instruction, operands)) || instruction.length == 0) { ++offset; continue; }
                for (std::uint8_t i = 0; i < instruction.operand_count; ++i)
                {
                    const auto& operand = operands[i];
                    if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (operand.imm.is_relative || operand.imm.is_address) && operand.imm.size)
                    {
                        const std::size_t begin = offset + operand.imm.offset, count = operand.imm.size / 8; for (std::size_t j = 0; j < count && begin + j < wildcard.size(); ++j) wildcard[begin + j] = true;
                    }
                    else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP && operand.mem.disp.size)
                    {
                        const std::size_t begin = offset + operand.mem.disp.offset, count = operand.mem.disp.size / 8; for (std::size_t j = 0; j < count && begin + j < wildcard.size(); ++j) wildcard[begin + j] = true;
                    }
                }
                offset += instruction.length;
            }
        }
#else
        (void)wildcardRelocations;
#endif
        std::ostringstream out;
        for (std::size_t i = 0; i < bytes.size(); ++i) { if (i) out << ' '; if (wildcard[i]) out << "??"; else out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]); }
        error.clear(); return out.str();
    }
}
''')

# Writable mapping metadata is needed by the generic scanner.
replace_once("include/quartz/client/native/NativeTypes.hpp", "        bool Readable = false;\n        bool Executable = false;\n", "        bool Readable = false;\n        bool Writable = false;\n        bool Executable = false;\n")
replace_once("src/runtime/RuntimeNative.cpp", "            region.Readable = !permissions.empty() && permissions[0] == 'r';\n            region.Executable = permissions.size() > 2 && permissions[2] == 'x';\n", "            region.Readable = !permissions.empty() && permissions[0] == 'r';\n            region.Writable = permissions.size() > 1 && permissions[1] == 'w';\n            region.Executable = permissions.size() > 2 && permissions[2] == 'x';\n")

write("include/quartz/client/ui/pages/MemoryScannerPage.hpp", r'''#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryScanner.hpp"

namespace quartz::client::ui
{
    class MemoryScannerPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "memory-scanner"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Memory Scanner"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    private:
        MemoryScanner _scanner;
        std::vector<RuntimeProcessInfo> _processes;
        pid_t _pid = 0;
        int _type = static_cast<int>(MemoryScanValueType::I32);
        int _comparison = static_cast<int>(MemoryScanComparison::Exact);
        std::array<char, 256> _valueA{"100"};
        std::array<char, 256> _valueB{};
        std::array<char, 32> _rangeStart{};
        std::array<char, 32> _rangeEnd{};
        bool _writableOnly = true;
        bool _executableOnly = false;
        bool _aligned = true;
        bool _caseSensitive = true;
        bool _wildcardRelocations = true;
        std::string _status;
        std::string _derivedPattern;
    };
}
''')

write("src/ui/pages/MemoryScannerPage.cpp", r'''#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    namespace
    {
        bool parseAddress(const char* text, std::uintptr_t& value)
        {
            if (!text || !*text) return false;
            std::string_view view(text); int base = 10;
            if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; }
            const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size();
        }
    }

    void MemoryScannerPage::render(PageContext& context, PageManager& manager)
    {
        _scanner.poll();
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("Cheat-Engine-style value scanning backed by process_vm_readv() and the shared Quartz worker pool. New Scan snapshots candidates; Next Scan filters the previous candidate bitmap without storing millions of uintptr_t values.");
        if (ImGui::Button("Refresh processes")) _processes = enumerateRuntimeProcesses();
        ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }
        if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>"))
        {
            for (const auto& process : _processes) { const bool active = process.Pid == _pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]"; if (ImGui::Selectable(label.c_str(), active)) _pid = process.Pid; if (active) ImGui::SetItemDefaultFocus(); }
            ImGui::EndCombo();
        }

        ImGui::SeparatorText("Scan");
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("Value type", memoryScanValueTypeName(static_cast<MemoryScanValueType>(_type))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanValueType::ByteArray); ++i) if (ImGui::Selectable(memoryScanValueTypeName(static_cast<MemoryScanValueType>(i)), i == _type)) _type = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Comparison", memoryScanComparisonName(static_cast<MemoryScanComparison>(_comparison))))
        {
            for (int i = 0; i <= static_cast<int>(MemoryScanComparison::ChangedFromTo); ++i) if (ImGui::Selectable(memoryScanComparisonName(static_cast<MemoryScanComparison>(i)), i == _comparison)) _comparison = i;
            ImGui::EndCombo();
        }
        const auto comparison = static_cast<MemoryScanComparison>(_comparison);
        const bool needsA = comparison == MemoryScanComparison::Exact || comparison == MemoryScanComparison::NotEqual || comparison == MemoryScanComparison::IncreasedBy || comparison == MemoryScanComparison::DecreasedBy || comparison == MemoryScanComparison::Greater || comparison == MemoryScanComparison::Less || comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        const bool needsB = comparison == MemoryScanComparison::Between || comparison == MemoryScanComparison::ChangedFromTo;
        if (needsA) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? "From" : "Value", _valueA.data(), _valueA.size()); }
        if (needsB) { ImGui::SetNextItemWidth(360.0f); ImGui::InputText(comparison == MemoryScanComparison::ChangedFromTo ? "To" : "Value B", _valueB.data(), _valueB.size()); }
        if (_type == static_cast<int>(MemoryScanValueType::ByteArray)) ImGui::TextDisabled("Byte arrays use Quartz hex syntax, including ?? and nibble wildcards A? / ?F.");
        ImGui::Checkbox("Writable mappings only", &_writableOnly); ImGui::SameLine(); ImGui::Checkbox("Executable only", &_executableOnly); ImGui::SameLine(); ImGui::Checkbox("Aligned values", &_aligned);
        if (_type == static_cast<int>(MemoryScanValueType::Utf8String) || _type == static_cast<int>(MemoryScanValueType::Utf16String)) { ImGui::SameLine(); ImGui::Checkbox("Case sensitive", &_caseSensitive); }

        MemoryScanRequest request{_pid, static_cast<MemoryScanValueType>(_type), comparison, _valueA.data(), _valueB.data(), _writableOnly, _executableOnly, _aligned, _caseSensitive};
        if (_scanner.running()) ImGui::BeginDisabled();
        if (ImGui::Button("New Scan")) _scanner.newScan(request, _status);
        ImGui::SameLine();
        if (!_scanner.hasSnapshot()) ImGui::BeginDisabled();
        if (ImGui::Button("Next Scan")) _scanner.nextScan(request, _status);
        if (!_scanner.hasSnapshot()) ImGui::EndDisabled();
        if (_scanner.running()) ImGui::EndDisabled();
        ImGui::SameLine(); if (!_scanner.running()) ImGui::BeginDisabled(); if (ImGui::Button("Cancel")) _scanner.cancel(); if (!_scanner.running()) ImGui::EndDisabled();

        const auto stats = _scanner.stats();
        if (stats.Running) drawIndeterminateProgressBar(ImVec2(360.0f, 0.0f));
        if (stats.MiBs > 0.0) { if (stats.MiBs >= 1024.0) ImGui::Text("%.2f GiB/s", stats.MiBs / 1024.0); else ImGui::Text("%.1f MiB/s", stats.MiBs); ImGui::SameLine(); ImGui::TextDisabled("%.1f MiB in %.3f s", stats.Bytes / (1024.0 * 1024.0), stats.Seconds); }
        ImGui::Text("Candidates: %llu", static_cast<unsigned long long>(stats.Candidates)); ImGui::SameLine(); ImGui::TextDisabled("%s%s", stats.Status.c_str(), _status.empty() ? "" : (" | " + _status).c_str());

        const auto rows = _scanner.results(256);
        if (!rows.empty() && ImGui::BeginTable("MemoryScanResults", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 260.0f)))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.0f); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 62.0f); ImGui::TableHeadersRow();
            for (const auto& row : rows)
            {
                ImGui::PushID(static_cast<int>(row.Address & 0x7fffffffULL)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(row.Address)); ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Value.c_str()); ImGui::TableNextColumn();
                if (ImGui::SmallButton("Inspect")) { auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = _pid; inspector.Address = row.Address; runtimeRefreshMemoryInspector(inspector); manager.open("native"); }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (stats.Candidates > rows.size()) ImGui::TextDisabled("Showing first %zu of %llu candidates.", rows.size(), static_cast<unsigned long long>(stats.Candidates));
        }

        ImGui::SeparatorText("Derive byte pattern from address range");
        ImGui::TextDisabled("Reads [start, end) and optionally wildcards relative immediates plus RIP-relative displacements decoded by Zydis.");
        ImGui::SetNextItemWidth(180.0f); ImGui::InputText("Start", _rangeStart.data(), _rangeStart.size()); ImGui::SameLine(); ImGui::SetNextItemWidth(180.0f); ImGui::InputText("End", _rangeEnd.data(), _rangeEnd.size());
        ImGui::Checkbox("Wildcard relocation-sensitive operands", &_wildcardRelocations);
        if (ImGui::Button("Derive pattern"))
        {
            std::uintptr_t start = 0, end = 0;
            if (!parseAddress(_rangeStart.data(), start) || !parseAddress(_rangeEnd.data(), end)) _status = "invalid range";
            else _derivedPattern = deriveRuntimeBytePattern(_pid, start, end, _wildcardRelocations, _status);
        }
        if (!_derivedPattern.empty())
        {
            ImGui::InputTextMultiline("Derived pattern", _derivedPattern.data(), _derivedPattern.size() + 1, ImVec2(-1.0f, 90.0f), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Copy pattern")) ImGui::SetClipboardText(_derivedPattern.c_str()); ImGui::SameLine();
            if (ImGui::Button("Create native-address binding"))
            {
                auto& binding = context.runtimeBindings.add(); std::snprintf(binding.Name, sizeof(binding.Name), "%s", "Derived signature"); binding.Source = RuntimeSourceKind::NativeAddress; binding.ProcessId = _pid; binding.AddressMode = ProcessAddressMode::Signature; binding.SignaturePatternKind = RuntimeSignaturePatternKind::HexadecimalPattern; binding.SignatureExecutableOnly = true; binding.WriteMaterial = false; binding.Clamp = false; binding.SmoothingHz = 0.0f; std::snprintf(binding.Signature, sizeof(binding.Signature), "%s", _derivedPattern.c_str()); _status = "created NativeAddress binding";
            }
        }
    }
}
''')


# ---------------------------------------------------------------------------
# Hardware data watchpoints (who writes / reads this address)
# ---------------------------------------------------------------------------
write("include/quartz/client/native/MemoryWatch.hpp", r'''#pragma once
#include "quartz/client/native/NativeTypes.hpp"
#include <memory>

namespace quartz::client
{
    enum class MemoryWatchAccess : int { Write, ReadWrite };
    struct MemoryWatchHit { double Time = 0.0; pid_t Tid = 0; std::uintptr_t Rip = 0; std::uintptr_t InstructionAddress = 0; std::string Instruction; };
    struct MemoryWatchState;

    class MemoryWatch
    {
    public:
        MemoryWatch() = default;
        ~MemoryWatch();
        MemoryWatch(const MemoryWatch&) = delete;
        MemoryWatch& operator=(const MemoryWatch&) = delete;
        bool start(pid_t pid, std::uintptr_t address, std::size_t size, MemoryWatchAccess access, std::size_t maxHits, std::string& error);
        void stop() noexcept;
        void clearHits();
        [[nodiscard]] bool running() const noexcept;
        [[nodiscard]] std::string status() const;
        [[nodiscard]] std::vector<MemoryWatchHit> hits() const;
    private:
        std::shared_ptr<MemoryWatchState> _state;
    };
}
''')

write("src/native/MemoryWatch.cpp", r'''#include "quartz/client/native/MemoryWatch.hpp"
#include "quartz/client/Model.hpp"

namespace quartz::client
{
    struct MemoryWatchState
    {
        std::mutex Mutex;
        bool Finished = false;
        std::string Status;
        std::vector<MemoryWatchHit> Hits;
        std::jthread Worker; // last: joins before state above is destroyed
    };

    namespace
    {
        std::string previousInstruction(const pid_t pid, const std::uintptr_t rip, std::uintptr_t& instructionAddress)
        {
            instructionAddress = 0;
            if (rip < 15) return {};
            std::array<std::uint8_t, 15> bytes{}; std::string error;
            if (!readProcessMemoryBlock(pid, rip - bytes.size(), bytes, error)) return {};
            std::string best;
            for (std::size_t length = 15; length > 0; --length)
            {
                const std::size_t offset = bytes.size() - length; std::string text; std::size_t decoded = 0;
                if (runtimeDecodeInstructionText(std::span<const std::uint8_t>(bytes).subspan(offset), rip - length, text, decoded) && decoded == length) { best = std::move(text); instructionAddress = rip - length; break; }
            }
            return best;
        }

        std::uint64_t lengthCode(const std::size_t size) noexcept { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 3 : 2; }
    }

    MemoryWatch::~MemoryWatch() { stop(); }
    bool MemoryWatch::running() const noexcept { if (!_state) return false; std::lock_guard lock(_state->Mutex); return !_state->Finished; }
    std::string MemoryWatch::status() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Status; }
    std::vector<MemoryWatchHit> MemoryWatch::hits() const { if (!_state) return {}; std::lock_guard lock(_state->Mutex); return _state->Hits; }
    void MemoryWatch::clearHits() { if (!_state) return; std::lock_guard lock(_state->Mutex); _state->Hits.clear(); }
    void MemoryWatch::stop() noexcept { if (!_state) return; _state->Worker.request_stop(); _state.reset(); }

    bool MemoryWatch::start(const pid_t pid, const std::uintptr_t address, const std::size_t size, const MemoryWatchAccess access, const std::size_t maxHits, std::string& error)
    {
        stop();
        if (pid <= 0 || address == 0) { error = "select a process and enter an address"; return false; }
        if (size != 1 && size != 2 && size != 4 && size != 8) { error = "hardware watchpoint size must be 1, 2, 4 or 8 bytes"; return false; }
        if ((address & (size - 1)) != 0) { error = "hardware watchpoint address must be aligned to its size"; return false; }
        auto state = std::make_shared<MemoryWatchState>(); MemoryWatchState* watch = state.get(); state->Status = "arming hardware watchpoint";
        state->Worker = std::jthread([watch, pid, address, size, access, maxHits](std::stop_token stop)
        {
            struct Thread { pid_t Tid = 0; bool Attached = false; bool Stopped = false; bool Armed = false; std::uint64_t Dr0 = 0, Dr6 = 0, Dr7 = 0; };
            constexpr std::size_t Dr0Offset = offsetof(struct user, u_debugreg[0]), Dr6Offset = offsetof(struct user, u_debugreg[6]), Dr7Offset = offsetof(struct user, u_debugreg[7]);
            std::vector<Thread> threads; std::size_t hits = 0; std::string lastError;
            auto setStatus = [&](std::string value) { std::lock_guard lock(watch->Mutex); watch->Status = std::move(value); };
            auto gone = [](Thread& thread) { thread.Attached = thread.Stopped = thread.Armed = false; };
            auto restore = [&](Thread& thread) { if (!thread.Armed || !thread.Stopped) return; runtimePtracePokeUser(thread.Tid, Dr7Offset, thread.Dr7); runtimePtracePokeUser(thread.Tid, Dr0Offset, thread.Dr0); runtimePtracePokeUser(thread.Tid, Dr6Offset, thread.Dr6); thread.Armed = false; };
            auto detach = [&](Thread& thread)
            {
                if (!thread.Attached) return;
                if (!thread.Stopped)
                {
                    if (::ptrace(PTRACE_INTERRUPT, thread.Tid, nullptr, nullptr) == 0) { int status = 0; if (runtimeWaitForPtraceStop(thread.Tid, stop, 0.25, status)) thread.Stopped = true; }
                }
                if (thread.Stopped) { restore(thread); if (::ptrace(PTRACE_DETACH, thread.Tid, nullptr, nullptr) == 0) { thread.Attached = thread.Stopped = false; return; } }
                gone(thread);
            };
            auto tracked = [&](const pid_t tid) { return std::ranges::any_of(threads, [tid](const Thread& thread) { return thread.Tid == tid && thread.Attached; }); };
            auto arm = [&](const pid_t tid)
            {
                if (tracked(tid) || stop.stop_requested()) return;
                if (::ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0) { lastError = std::strerror(errno); return; }
                threads.push_back({}); Thread& thread = threads.back(); thread.Tid = tid; thread.Attached = true;
                if (::ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) { lastError = std::strerror(errno); detach(thread); return; }
                int status = 0; if (!runtimeWaitForPtraceStop(tid, stop, 0.25, status)) { lastError = "thread did not stop"; detach(thread); return; } thread.Stopped = true;
                if (!runtimePtracePeekUser(tid, Dr0Offset, thread.Dr0) || !runtimePtracePeekUser(tid, Dr6Offset, thread.Dr6) || !runtimePtracePeekUser(tid, Dr7Offset, thread.Dr7)) { lastError = std::strerror(errno); detach(thread); return; }
                std::uint64_t dr7 = thread.Dr7; dr7 &= ~0x3ULL; dr7 &= ~(0xFULL << 16); dr7 |= 1ULL;
                const std::uint64_t rw = access == MemoryWatchAccess::Write ? 1ULL : 3ULL; dr7 |= (rw | (lengthCode(size) << 2)) << 16;
                if (!runtimePtracePokeUser(tid, Dr0Offset, address) || !runtimePtracePokeUser(tid, Dr6Offset, 0) || !runtimePtracePokeUser(tid, Dr7Offset, dr7)) { lastError = std::strerror(errno); detach(thread); return; }
                thread.Armed = true; if (::ptrace(PTRACE_CONT, tid, nullptr, nullptr) == 0) thread.Stopped = false; else detach(thread);
            };
            auto refresh = [&] { for (const pid_t tid : enumerateRuntimeThreads(pid)) arm(tid); };
            refresh(); double nextRefresh = runtimeSteadySeconds() + 0.10, nextStatus = 0.0;
            while (!stop.stop_requested() && (maxHits == 0 || hits < maxHits))
            {
                const double now = runtimeSteadySeconds(); if (now >= nextRefresh) { refresh(); nextRefresh = now + 0.10; }
                bool observed = false;
                for (auto& thread : threads)
                {
                    if (!thread.Attached || thread.Stopped) continue;
                    int status = 0; errno = 0; const pid_t result = ::waitpid(thread.Tid, &status, __WALL | WNOHANG); if (result == 0) continue; if (result < 0) { if (errno == ESRCH || errno == ECHILD) gone(thread); continue; }
                    observed = true; if (WIFEXITED(status) || WIFSIGNALED(status)) { gone(thread); continue; } if (!WIFSTOPPED(status)) continue; thread.Stopped = true;
                    const int signal = WSTOPSIG(status); bool watched = false;
                    if (signal == SIGTRAP && thread.Armed)
                    {
                        std::uint64_t dr6 = 0; user_regs_struct regs{}; const bool haveDr6 = runtimePtracePeekUser(thread.Tid, Dr6Offset, dr6), haveRegs = ::ptrace(PTRACE_GETREGS, thread.Tid, nullptr, &regs) == 0;
                        watched = haveDr6 && (dr6 & 1ULL) != 0;
                        if (watched && haveRegs)
                        {
                            MemoryWatchHit hit; hit.Time = runtimeSteadySeconds(); hit.Tid = thread.Tid; hit.Rip = regs.rip; hit.Instruction = previousInstruction(pid, regs.rip, hit.InstructionAddress);
                            if (hit.Instruction.empty()) hit.Instruction = "<could not recover preceding instruction>";
                            { std::lock_guard lock(watch->Mutex); watch->Hits.push_back(std::move(hit)); if (watch->Hits.size() > 256) watch->Hits.erase(watch->Hits.begin()); hits = watch->Hits.size(); }
                        }
                        runtimePtracePokeUser(thread.Tid, Dr6Offset, 0);
                    }
                    const int deliver = signal == SIGTRAP || signal == SIGSTOP ? 0 : signal; if (::ptrace(PTRACE_CONT, thread.Tid, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(deliver))) == 0) thread.Stopped = false; else detach(thread);
                }
                if (now >= nextStatus) { std::ostringstream text; text << "watching 0x" << std::hex << address << std::dec << " | threads " << threads.size() << " | hits " << hits; if (!lastError.empty()) text << " | last attach error: " << lastError; setStatus(text.str()); nextStatus = now + 0.25; }
                if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (auto& thread : threads) detach(thread);
            { std::lock_guard lock(watch->Mutex); watch->Finished = true; watch->Status = stop.stop_requested() ? "watch stopped" : "watch hit limit reached"; }
        });
        _state = std::move(state); error.clear(); return true;
    }
}
''')

write("include/quartz/client/ui/pages/MemoryWatchPage.hpp", r'''#pragma once
#include "quartz/client/ui/Page.hpp"
#include "quartz/client/native/MemoryWatch.hpp"

namespace quartz::client::ui
{
    class MemoryWatchPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "memory-watch"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Memory Watch"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    private:
        MemoryWatch _watch;
        std::vector<RuntimeProcessInfo> _processes;
        pid_t _pid = 0;
        std::array<char, 32> _address{};
        int _size = 2;
        int _access = static_cast<int>(MemoryWatchAccess::Write);
        int _maxHits = 64;
        std::string _status;
    };
}
''')

write("src/ui/pages/MemoryWatchPage.cpp", r'''#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    namespace { bool parseWatchAddress(const char* text, std::uintptr_t& value) { if (!text || !*text) return false; std::string_view view(text); int base = 10; if (view.starts_with("0x") || view.starts_with("0X")) { view.remove_prefix(2); base = 16; } const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value, base); return ec == std::errc{} && ptr == view.data() + view.size(); } }
    void MemoryWatchPage::render(PageContext&, PageManager& manager)
    {
        if (_processes.empty()) _processes = enumerateRuntimeProcesses();
        ImGui::TextWrapped("Hardware data breakpoints answer the classic 'who writes/reads this address?' question. x86 provides write-only or read/write data traps (not read-only) and only four debug-address slots per thread; Quartz uses one slot and mirrors it onto newly created threads.");
        if (ImGui::Button("Refresh processes")) _processes = enumerateRuntimeProcesses(); ImGui::SameLine(); ImGui::SetNextItemWidth(420.0f);
        const RuntimeProcessInfo* selected = nullptr; for (const auto& process : _processes) if (process.Pid == _pid) { selected = &process; break; }
        if (ImGui::BeginCombo("Process", selected ? runtimeProcessDisplayTitle(*selected).c_str() : "<select process>")) { for (const auto& process : _processes) { const bool active = process.Pid == _pid; const std::string label = runtimeProcessDisplayTitle(process) + "  [" + std::to_string(process.Pid) + "]"; if (ImGui::Selectable(label.c_str(), active)) _pid = process.Pid; } ImGui::EndCombo(); }
        ImGui::SetNextItemWidth(210.0f); ImGui::InputText("Address", _address.data(), _address.size());
        ImGui::SameLine(); ImGui::SetNextItemWidth(110.0f); static constexpr const char* Sizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"}; ImGui::Combo("Size", &_size, Sizes, 4);
        ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); ImGui::Combo("Access", &_access, "Write\0Read / write\0");
        ImGui::SetNextItemWidth(120.0f); ImGui::InputInt("Hit limit", &_maxHits); ImGui::SameLine(); ImGui::TextDisabled("0 = unlimited");
        if (_watch.running()) ImGui::BeginDisabled();
        if (ImGui::Button("Start watch")) { std::uintptr_t address = 0; static constexpr std::size_t Widths[] = {1, 2, 4, 8}; if (!parseWatchAddress(_address.data(), address)) _status = "invalid address"; else _watch.start(_pid, address, Widths[std::clamp(_size, 0, 3)], static_cast<MemoryWatchAccess>(_access), static_cast<std::size_t>(std::max(_maxHits, 0)), _status); }
        if (_watch.running()) ImGui::EndDisabled(); ImGui::SameLine(); if (!_watch.running()) ImGui::BeginDisabled(); if (ImGui::Button("Stop")) _watch.stop(); if (!_watch.running()) ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Clear hits")) _watch.clearHits();
        const std::string watchStatus = _watch.status(); if (!watchStatus.empty()) ImGui::TextWrapped("%s", watchStatus.c_str()); if (!_status.empty()) ImGui::TextDisabled("%s", _status.c_str());
        const auto hits = _watch.hits();
        if (ImGui::BeginTable("MemoryWatchHits", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0.0f, 360.0f)))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 42.0f); ImGui::TableSetupColumn("TID", ImGuiTableColumnFlags_WidthFixed, 75.0f); ImGui::TableSetupColumn("RIP after access", ImGuiTableColumnFlags_WidthFixed, 145.0f); ImGui::TableSetupColumn("Best-effort previous instruction"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 62.0f); ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < hits.size(); ++i) { const auto& hit = hits[i]; ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1); ImGui::TableNextColumn(); ImGui::Text("%d", hit.Tid); ImGui::TableNextColumn(); ImGui::Text("0x%llX", static_cast<unsigned long long>(hit.Rip)); ImGui::TableNextColumn(); if (hit.InstructionAddress) ImGui::Text("0x%llX  %s", static_cast<unsigned long long>(hit.InstructionAddress), hit.Instruction.c_str()); else ImGui::TextUnformatted(hit.Instruction.c_str()); ImGui::TableNextColumn(); if (ImGui::SmallButton("Inspect")) { auto& inspector = runtimeMemoryInspectorState(); inspector.Pid = _pid; inspector.Address = hit.InstructionAddress ? hit.InstructionAddress : hit.Rip; runtimeRefreshMemoryInspector(inspector); manager.open("native"); } ImGui::PopID(); }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Data breakpoints trap after the memory access. Quartz backtracks up to 15 bytes and shows a best-effort preceding instruction; RIP itself is the post-access instruction pointer.");
    }
}
''')


# ---------------------------------------------------------------------------
# Intel-syntax opcode pattern editor + linting
# ---------------------------------------------------------------------------
write("include/quartz/client/native/OpcodePatternEditor.hpp", r'''#pragma once
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"

namespace quartz::client
{
    struct OpcodePatternEditorState
    {
        TextEditor Editor;
        std::uint64_t BindingId = 0;
        bool Initialized = false;
        std::string Status;
        std::vector<std::string> Diagnostics;
    };
    OpcodePatternEditorState& opcodePatternEditorState();
    const TextEditor::Language* intelAsmPatternLanguage();
    void openOpcodePatternEditor(RuntimeBinding& binding);
    bool lintOpcodePattern(OpcodePatternEditorState& state);
    bool applyOpcodePattern(OpcodePatternEditorState& state, RuntimeBindingEngine& engine);
}
''')

write("src/native/OpcodePatternEditor.cpp", r'''#include "quartz/client/native/OpcodePatternEditor.hpp"
#include "quartz/client/Model.hpp"
#if QUARTZ_HAS_ZYDIS
#include <Zydis/Zydis.h>
#endif

namespace quartz::client
{
    namespace
    {
        TextEditor::Iterator asmIdentifier(TextEditor::Iterator start, const TextEditor::Iterator end)
        {
            auto it = start; if (it == end || !(std::isalpha(static_cast<unsigned char>(*it)) || *it == '_' || *it == '.')) return start; ++it;
            while (it != end && (std::isalnum(static_cast<unsigned char>(*it)) || *it == '_' || *it == '.')) ++it; return it;
        }
        TextEditor::Iterator asmNumber(TextEditor::Iterator start, const TextEditor::Iterator end)
        {
            auto it = start; if (it == end || !std::isdigit(static_cast<unsigned char>(*it))) return start; ++it;
            while (it != end && (std::isxdigit(static_cast<unsigned char>(*it)) || *it == 'x' || *it == 'X' || *it == 'h' || *it == 'H' || *it == '.')) ++it; return it;
        }
    }

    const TextEditor::Language* intelAsmPatternLanguage()
    {
        static const TextEditor::Language language = []
        {
            TextEditor::Language l; l.name = "Intel x86-64 pattern"; l.caseSensitive = false; l.singleLineComment = ";"; l.singleLineCommentAlt = "//"; l.hasSingleQuotedStrings = true; l.hasDoubleQuotedStrings = true; l.getIdentifier = asmIdentifier; l.getNumber = asmNumber;
            l.isPunctuation = [](const ImWchar c) { return std::string_view("[]()+-*/,?:").find(static_cast<char>(c)) != std::string_view::npos; };
#if QUARTZ_HAS_ZYDIS
            for (int i = 0; i <= static_cast<int>(ZYDIS_MNEMONIC_MAX_VALUE); ++i) if (const char* name = ZydisMnemonicGetString(static_cast<ZydisMnemonic>(i)); name && *name) l.keywords.insert(runtimeLower(name));
#else
            l.keywords = {"mov", "lea", "call", "jmp", "cmp", "test", "add", "sub", "imul", "xor", "and", "or", "push", "pop", "ret", "nop"};
#endif
            l.declarations = {"byte", "word", "dword", "qword", "xmmword", "ymmword", "zmmword", "ptr", "short", "near", "far"};
            static constexpr const char* regs[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","rip","eax","ebx","ecx","edx","esi","edi","ebp","esp","ax","bx","cx","dx","si","di","bp","sp","al","bl","cl","dl","ah","bh","ch","dh","spl","bpl","sil","dil","cs","ds","es","fs","gs","ss"};
            for (const char* reg : regs) l.identifiers.insert(reg); for (int i = 0; i < 32; ++i) { l.identifiers.insert("r" + std::to_string(i)); l.identifiers.insert("r" + std::to_string(i) + "d"); l.identifiers.insert("r" + std::to_string(i) + "w"); l.identifiers.insert("r" + std::to_string(i) + "b"); l.identifiers.insert("xmm" + std::to_string(i)); l.identifiers.insert("ymm" + std::to_string(i)); l.identifiers.insert("zmm" + std::to_string(i)); }
            return l;
        }();
        return &language;
    }

    OpcodePatternEditorState& opcodePatternEditorState() { static OpcodePatternEditorState state; return state; }

    void openOpcodePatternEditor(RuntimeBinding& binding)
    {
        auto& state = opcodePatternEditorState(); state.BindingId = binding.Id; state.Editor.SetLanguage(intelAsmPatternLanguage()); state.Editor.SetShowLineNumbersEnabled(true); state.Editor.SetShowMiniMapEnabled(true); state.Editor.SetText(binding.Signature); state.Initialized = true; state.Status.clear(); lintOpcodePattern(state);
    }

    bool lintOpcodePattern(OpcodePatternEditorState& state)
    {
        state.Diagnostics.clear(); state.Editor.ClearMarkers(); const std::string text = state.Editor.GetText(); std::istringstream stream(text); std::string line; std::size_t lineNumber = 0;
        const auto* language = intelAsmPatternLanguage();
        while (std::getline(stream, line))
        {
            std::string normalized = runtimeNormalizeOpcodeText(line); if (normalized.empty() || normalized.starts_with(";") || normalized.starts_with("#") || normalized.starts_with("//")) { ++lineNumber; continue; }
            int brackets = 0; for (const char c : normalized) { if (c == '[') ++brackets; else if (c == ']') --brackets; if (brackets < 0) break; }
            std::string message;
            if (brackets != 0) message = "unbalanced memory operand brackets";
            else
            {
                const std::size_t split = normalized.find_first_of(" \t"); const std::string mnemonic = normalized.substr(0, split);
                if (mnemonic.find('*') == std::string::npos && mnemonic.find('?') == std::string::npos && !language->keywords.contains(mnemonic)) message = "unknown x86 mnemonic: " + mnemonic;
            }
            if (!message.empty()) { state.Diagnostics.push_back("line " + std::to_string(lineNumber + 1) + ": " + message); state.Editor.AddMarker(lineNumber, IM_COL32(255, 90, 90, 255), IM_COL32(255, 135, 135, 255), message, message); }
            ++lineNumber;
        }
        state.Status = state.Diagnostics.empty() ? "Pattern syntax looks valid" : std::to_string(state.Diagnostics.size()) + " lint issue(s)"; return state.Diagnostics.empty();
    }

    bool applyOpcodePattern(OpcodePatternEditorState& state, RuntimeBindingEngine& engine)
    {
        RuntimeBinding* binding = engine.findBinding(state.BindingId); if (!binding) { state.Status = "target binding no longer exists"; return false; }
        const std::string text = state.Editor.GetText(); if (text.size() >= sizeof(binding->Signature)) { state.Status = "opcode pattern exceeds binding storage"; return false; }
        lintOpcodePattern(state); std::snprintf(binding->Signature, sizeof(binding->Signature), "%s", text.c_str()); binding->SignaturePatternKind = RuntimeSignaturePatternKind::OpcodePattern; binding->AddressMode = ProcessAddressMode::Signature; resetRuntimeSignatureScan(*binding); binding->SignatureConfigHash = 0; binding->NextUpdate = 0.0; engine.markChanged(); state.Status = state.Diagnostics.empty() ? "Applied to binding and scheduled rescan" : "Applied with lint warnings and scheduled rescan"; return true;
    }
}
''')

write("include/quartz/client/ui/pages/OpcodeEditorPage.hpp", r'''#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class OpcodeEditorPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "opcode-editor"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "Opcode Pattern Editor"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        [[nodiscard]] PagePresentation presentation() const noexcept override { return PagePresentation::Standalone; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
''')

write("src/ui/pages/OpcodeEditorPage.cpp", r'''#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void OpcodeEditorPage::render(PageContext& context, PageManager& manager)
    {
        auto& state = opcodePatternEditorState();
        if (!state.Initialized) { ImGui::TextDisabled("Open an opcode-pattern native binding from Native / Memory first."); if (ImGui::Button("Back")) manager.closeStandalone(); return; }
        if (ImGui::Button("Back")) manager.closeStandalone(); ImGui::SameLine(); if (ImGui::Button("Lint")) lintOpcodePattern(state); ImGui::SameLine(); if (ImGui::Button("Apply to binding")) applyOpcodePattern(state, context.runtimeBindings); ImGui::SameLine(); ImGui::TextDisabled("%s", state.Status.c_str());
        ImGui::TextWrapped("Intel-syntax opcode pattern editor. Mnemonics/registers are highlighted; * matches arbitrary text and ? matches one character in Quartz's decoded-instruction matcher. Linting checks mnemonics and operand bracket structure without pretending wildcard patterns are assemblable source code.");
        state.Editor.Render("OpcodePatternEditor", ImVec2(0.0f, -90.0f), ImGuiChildFlags_Borders);
        if (!state.Diagnostics.empty()) { ImGui::SeparatorText("Diagnostics"); for (const auto& diagnostic : state.Diagnostics) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", diagnostic.c_str()); }
    }
}
''')

# Native page gains an opcode-editor launch point while keeping the shared inspector.
write("src/ui/pages/NativePage.cpp", r'''#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/native/OpcodePatternEditor.hpp"

namespace quartz::client::ui
{
    void NativePage::render(PageContext& context, PageManager& manager)
    {
        auto& inspector = runtimeMemoryInspectorState(); auto& engine = context.runtimeBindings;
        ImGui::TextWrapped("Native-process workspace: active native bindings, signature telemetry and the shared memory/disassembly inspector. Generic value scans and hardware data breakpoints have dedicated pages now.");
        if (ImGui::Button("Memory Scanner...")) manager.open("memory-scanner"); ImGui::SameLine(); if (ImGui::Button("Memory Watch...")) manager.open("memory-watch");
        ImGui::SeparatorText("Native bindings");
        bool any = false;
        for (auto& binding : engine.bindings())
        {
            if (binding.Source != RuntimeSourceKind::NativeProcess && binding.Source != RuntimeSourceKind::NativeAddress) continue; any = true; ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
            ImGui::Text("%s", binding.Name); ImGui::SameLine(); ImGui::TextDisabled("PID %d", binding.ProcessId);
            if (binding.AddressMode == ProcessAddressMode::Signature)
            {
                ImGui::SameLine(); if (binding.SignatureScanRunning) ImGui::TextDisabled("scanning"); else if (binding.SignatureResolvedAddress) ImGui::TextDisabled("resolved 0x%llX", static_cast<unsigned long long>(binding.SignatureResolvedAddress)); else if (!binding.SignatureStatus.empty()) ImGui::TextDisabled("%s", binding.SignatureStatus.c_str());
                if (binding.SignatureScanAverageMiBs > 0.0) { if (binding.SignatureScanAverageMiBs >= 1024.0) ImGui::TextDisabled("Average scan speed: %.2f GiB/s", binding.SignatureScanAverageMiBs / 1024.0); else ImGui::TextDisabled("Average scan speed: %.1f MiB/s", binding.SignatureScanAverageMiBs); if (binding.SignatureScanLastSeconds > 0.0) { ImGui::SameLine(); ImGui::TextDisabled("last %.1f MiB / %.3f s", binding.SignatureScanLastBytes / (1024.0 * 1024.0), binding.SignatureScanLastSeconds); } }
                if (ImGui::SmallButton("Rescan")) { resetRuntimeSignatureScan(binding); binding.SignatureConfigHash = 0; binding.NextUpdate = 0.0; }
                if (binding.SignaturePatternKind == RuntimeSignaturePatternKind::OpcodePattern) { ImGui::SameLine(); if (ImGui::SmallButton("Opcode editor...")) { openOpcodePatternEditor(binding); manager.open("opcode-editor"); } }
            }
            ImGui::Separator(); ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("No NativeProcess/NativeAddress bindings configured.");
        drawRuntimeMemoryInspector(inspector);
    }
}
''')


# ---------------------------------------------------------------------------
# Page registration
# ---------------------------------------------------------------------------
replace_once("src/ui/PageManager.cpp", '#include "quartz/client/ui/pages/RGBPage.hpp"\n', '#include "quartz/client/ui/pages/RGBPage.hpp"\n#include "quartz/client/ui/pages/ShadersPage.hpp"\n')
replace_once("src/ui/PageManager.cpp", '#include "quartz/client/ui/pages/NativePage.hpp"\n', '#include "quartz/client/ui/pages/NativePage.hpp"\n#include "quartz/client/ui/pages/MemoryScannerPage.hpp"\n#include "quartz/client/ui/pages/MemoryWatchPage.hpp"\n')
replace_once("src/ui/PageManager.cpp", '#include "quartz/client/ui/pages/ShaderEditorPage.hpp"\n', '#include "quartz/client/ui/pages/ShaderEditorPage.hpp"\n#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"\n')
replace_once("src/ui/PageManager.cpp", "        manager.add<RGBPage>();\n", "        manager.add<RGBPage>();\n        manager.add<ShadersPage>();\n")
replace_once("src/ui/PageManager.cpp", "        manager.add<NativePage>();\n", "        manager.add<NativePage>();\n        manager.add<MemoryScannerPage>();\n        manager.add<MemoryWatchPage>();\n")
replace_once("src/ui/PageManager.cpp", "        manager.add<ShaderEditorPage>();\n", "        manager.add<ShaderEditorPage>();\n        manager.add<OpcodeEditorPage>();\n")


# ---------------------------------------------------------------------------
# Binding/control readability + cramped register-capture layout
# ---------------------------------------------------------------------------
# The register-capture displacement controls no longer fight on a single line.
replace_once("src/ui/RuntimeUI.cpp", r'''                    if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual)
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
''', r'''                    ImGui::Indent(18.0f);
                    ImGui::SetNextItemWidth(220.0f);
                    if (binding.SignatureDisplacementType == RuntimeDisplacementType::Manual) changed |= ImGui::InputInt("Manual displacement", &binding.SignatureManualDisplacement);
                    else changed |= ImGui::InputInt("Displacement byte offset", &binding.SignatureRegisterDisplacementOffset);
                    ImGui::Unindent(18.0f);
''')

# Comparator controls get a vertical form rather than a cramped SameLine chain.
replace_once("src/ui/RuntimeUI.cpp", r'''            int condition = static_cast<int>(binding.CompareCondition);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Compare", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { binding.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("A", &binding.CompareA, 0.01f);
            if (binding.CompareCondition == RuntimeCompareCondition::Between || binding.CompareCondition == RuntimeCompareCondition::Outside) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("B", &binding.CompareB, 0.01f); }
            if (binding.CompareCondition == RuntimeCompareCondition::Equal || binding.CompareCondition == RuntimeCompareCondition::NotEqual) { ImGui::SameLine(); ImGui::SetNextItemWidth(150.0f); changed |= ImGui::DragFloat("Tolerance", &binding.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
            int result = static_cast<int>(binding.CompareResult);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Result", &result, "Any\0All\0None\0Count\0Fraction\0First match index\0")) { binding.CompareResult = static_cast<RuntimeMassCompareResult>(result); changed = true; }
''', r'''            int condition = static_cast<int>(binding.CompareCondition);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Condition", &condition, "==\0!=\0<\0<=\0>\0>=\0between\0outside\0")) { binding.CompareCondition = static_cast<RuntimeCompareCondition>(condition); changed = true; }
            ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Compare value A", &binding.CompareA, 0.01f);
            if (binding.CompareCondition == RuntimeCompareCondition::Between || binding.CompareCondition == RuntimeCompareCondition::Outside) { ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Compare value B", &binding.CompareB, 0.01f); }
            if (binding.CompareCondition == RuntimeCompareCondition::Equal || binding.CompareCondition == RuntimeCompareCondition::NotEqual) { ImGui::SetNextItemWidth(220.0f); changed |= ImGui::DragFloat("Tolerance", &binding.CompareTolerance, 0.0001f, 0.000001f, 1000.0f, "%.6f"); }
            int result = static_cast<int>(binding.CompareResult);
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("Reduction result", &result, "Any\0All\0None\0Count\0Fraction\0First match index\0")) { binding.CompareResult = static_cast<RuntimeMassCompareResult>(result); changed = true; }
            ImGui::Spacing();
''')

# Give every binding/control an actual bordered card and breathing room.
replace_once("src/ui/RuntimeUI.cpp", r'''        auto drawOne = [&](RuntimeBinding& binding) { bool shouldErase = false; drawRuntimeBinding(engine, shaderFramebuffer, binding, shouldErase); if (shouldErase) erase = static_cast<std::size_t>(&binding - engine.bindings().data()); };
''', r'''        auto drawOne = [&](RuntimeBinding& binding)
        {
            bool shouldErase = false;
            ImGui::PushID(static_cast<int>(binding.Id & 0x7fffffffULL));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::BeginChild("##BindingCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawRuntimeBinding(engine, shaderFramebuffer, binding, shouldErase);
            ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (shouldErase) erase = static_cast<std::size_t>(&binding - engine.bindings().data());
        };
''')
replace_once("src/ui/RuntimeUI.cpp", r'''        auto drawOne = [&](RuntimeControlRule& control) { bool shouldErase = false; drawRuntimeControlRule(engine, shaderFramebuffer, control, shouldErase); if (shouldErase) erase = static_cast<std::size_t>(&control - engine.controls().data()); };
''', r'''        auto drawOne = [&](RuntimeControlRule& control)
        {
            bool shouldErase = false;
            ImGui::PushID(static_cast<int>(control.Id & 0x7fffffffULL));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::BeginChild("##ControlCard", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) drawRuntimeControlRule(engine, shaderFramebuffer, control, shouldErase);
            ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopID(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (shouldErase) erase = static_cast<std::size_t>(&control - engine.controls().data());
        };
''')

# Declare the shared progress helper for the new scanner page.
replace_once("include/quartz/client/Functions.hpp", "    void mapSpectrumToColumns(", "    void drawIndeterminateProgressBar(ImVec2 size);\n    void mapSpectrumToColumns(")

print("studio tools migration applied")
