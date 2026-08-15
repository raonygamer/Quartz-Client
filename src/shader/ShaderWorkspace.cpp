#include "quartz/client/shader/ShaderWorkspace.hpp"
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
