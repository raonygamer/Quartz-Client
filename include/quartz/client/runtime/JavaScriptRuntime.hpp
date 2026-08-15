#pragma once
#include "quartz/client/runtime/RuntimeTypes.hpp"

namespace quartz::client
{
    class RuntimeBindingEngine;
    class EvdevKeyboard;

    class JavaScriptRuntime
    {
    public:
        explicit JavaScriptRuntime(RuntimeBindingEngine& legacy);
        ~JavaScriptRuntime();

        std::vector<RuntimeScript>& scripts() noexcept { return _scripts; }
        const std::vector<RuntimeScript>& scripts() const noexcept { return _scripts; }
        RuntimeScriptSettings& settings() noexcept { return _settings; }
        const RuntimeScriptSettings& settings() const noexcept { return _settings; }
        RuntimeControlOutput& output() noexcept { return _output; }
        const RuntimeControlOutput& output() const noexcept { return _output; }
        const std::filesystem::path& path() const noexcept { return _path; }
        std::uint64_t revision() const noexcept { return _revision; }

        RuntimeScript& add();
        void erase(std::size_t index, RuntimeBindingEngine& legacy);
        RuntimeScript* find(std::uint64_t id) noexcept;
        const RuntimeScript* find(std::uint64_t id) const noexcept;
        RuntimeScript* findByName(std::string_view name) noexcept;
        const RuntimeScript* findByName(std::string_view name) const noexcept;

        void markChanged() noexcept { ++_revision; }
        bool save();
        void saveIfChanged() { if (_savedRevision != _revision) save(); }
        void syncProfile(RuntimeBindingEngine& legacy);
        void pollReloadHotkey(GLFWwindow* window, const EvdevKeyboard& keyboard);

    private:
        void load();
        void migrateLegacy(RuntimeBindingEngine& legacy, bool hadOwnFile);

        std::filesystem::path _path;
        std::vector<RuntimeScript> _scripts;
        RuntimeScriptSettings _settings{};
        RuntimeControlOutput _output{};
        std::uint64_t _nextScriptId = 1;
        std::uint64_t _revision = 0;
        std::uint64_t _savedRevision = 0;
        std::uint64_t _observedProfileId = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t _observedProfileRevision = std::numeric_limits<std::uint64_t>::max();
    };
}
