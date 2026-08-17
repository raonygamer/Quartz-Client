#pragma once
#include "quartz/client/runtime/RuntimeTypes.hpp"

namespace quartz::client
{
    class RuntimeBindingEngine;
    class EvdevKeyboard;

    class JavaScriptRuntime
    {
    public:
        JavaScriptRuntime();
        ~JavaScriptRuntime();

        std::vector<RuntimeScript>& scripts() noexcept { return _scripts; }
        const std::vector<RuntimeScript>& scripts() const noexcept { return _scripts; }
        RuntimeScriptSettings& settings() noexcept { return _settings; }
        const RuntimeScriptSettings& settings() const noexcept { return _settings; }
        RuntimeControlOutput& output() noexcept { return _output; }
        const RuntimeControlOutput& output() const noexcept { return _output; }
        RuntimeControlOutput& outputFor(std::uint64_t scriptId) { return _scriptOutputs[scriptId]; }
        const std::filesystem::path& path() const noexcept { return _path; }
        std::uint64_t revision() const noexcept { return _revision; }

        RuntimeScript& add();
        void erase(std::size_t index, RuntimeBindingEngine& runtime);
        RuntimeScript* find(std::uint64_t id) noexcept;
        const RuntimeScript* find(std::uint64_t id) const noexcept;
        RuntimeScript* findByName(std::string_view name) noexcept;
        const RuntimeScript* findByName(std::string_view name) const noexcept;

        bool lockShaderMutex(std::uint64_t scriptId) noexcept;
        bool unlockShaderMutex(std::uint64_t scriptId) noexcept;
        bool shaderMutexLocked() const noexcept { return _shaderMutexOwner != 0; }
        bool ownsShaderMutex(std::uint64_t scriptId) const noexcept { return _shaderMutexOwner == scriptId; }
        bool canWriteShader(std::uint64_t scriptId) const noexcept { return _shaderMutexOwner == 0 || _shaderMutexOwner == scriptId; }
        std::uint64_t shaderMutexOwner() const noexcept { return _shaderMutexOwner; }
        std::string shaderMutexOwnerDisplayName() const;

        void clearOutput(std::uint64_t scriptId) noexcept;
        void clearOutputs() noexcept;
        void rebuildOutput() noexcept;
        void markChanged() noexcept { ++_revision; }
        bool save();
        void saveIfChanged() { if (_savedRevision != _revision) save(); }
        void syncProfile(RuntimeBindingEngine& runtime);
        void pollReloadHotkey(GLFWwindow* window, const EvdevKeyboard& keyboard);

    private:
        void load();

        std::filesystem::path _path;
        std::vector<RuntimeScript> _scripts;
        RuntimeScriptSettings _settings{};
        std::unordered_map<std::uint64_t, RuntimeControlOutput> _scriptOutputs;
        RuntimeControlOutput _output{};
        std::uint64_t _shaderMutexOwner = 0;
        std::uint64_t _nextScriptId = 1;
        std::uint64_t _revision = 0;
        std::uint64_t _savedRevision = 0;
        std::uint64_t _observedProfileId = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t _observedProfileRevision = std::numeric_limits<std::uint64_t>::max();
    };
}
