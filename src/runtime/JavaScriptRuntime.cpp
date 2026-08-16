#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/input/Input.hpp"
#include "quartz/client/Model.hpp"
#include <fstream>

namespace quartz::client
{
    namespace
    {
        std::uint16_t evdevKeyFromGlfw(const int key) noexcept
        {
            switch (key)
            {
            case GLFW_KEY_F1: return KEY_F1; case GLFW_KEY_F2: return KEY_F2; case GLFW_KEY_F3: return KEY_F3; case GLFW_KEY_F4: return KEY_F4; case GLFW_KEY_F5: return KEY_F5; case GLFW_KEY_F6: return KEY_F6;
            case GLFW_KEY_F7: return KEY_F7; case GLFW_KEY_F8: return KEY_F8; case GLFW_KEY_F9: return KEY_F9; case GLFW_KEY_F10: return KEY_F10; case GLFW_KEY_F11: return KEY_F11; case GLFW_KEY_F12: return KEY_F12;
            case GLFW_KEY_A: return KEY_A; case GLFW_KEY_B: return KEY_B; case GLFW_KEY_C: return KEY_C; case GLFW_KEY_D: return KEY_D; case GLFW_KEY_E: return KEY_E; case GLFW_KEY_F: return KEY_F; case GLFW_KEY_G: return KEY_G; case GLFW_KEY_H: return KEY_H; case GLFW_KEY_I: return KEY_I; case GLFW_KEY_J: return KEY_J;
            case GLFW_KEY_K: return KEY_K; case GLFW_KEY_L: return KEY_L; case GLFW_KEY_M: return KEY_M; case GLFW_KEY_N: return KEY_N; case GLFW_KEY_O: return KEY_O; case GLFW_KEY_P: return KEY_P; case GLFW_KEY_Q: return KEY_Q; case GLFW_KEY_R: return KEY_R; case GLFW_KEY_S: return KEY_S; case GLFW_KEY_T: return KEY_T; case GLFW_KEY_U: return KEY_U; case GLFW_KEY_V: return KEY_V; case GLFW_KEY_W: return KEY_W; case GLFW_KEY_X: return KEY_X; case GLFW_KEY_Y: return KEY_Y; case GLFW_KEY_Z: return KEY_Z;
            case GLFW_KEY_0: return KEY_0; case GLFW_KEY_1: return KEY_1; case GLFW_KEY_2: return KEY_2; case GLFW_KEY_3: return KEY_3; case GLFW_KEY_4: return KEY_4; case GLFW_KEY_5: return KEY_5; case GLFW_KEY_6: return KEY_6; case GLFW_KEY_7: return KEY_7; case GLFW_KEY_8: return KEY_8; case GLFW_KEY_9: return KEY_9;
            default: return 0;
            }
        }

        template<std::size_t N>
        void copyField(char (&destination)[N], const std::string& value) { std::snprintf(destination, N, "%s", value.c_str()); }

        std::vector<std::string> fieldsOf(const std::string& line)
        {
            std::vector<std::string> fields;
            std::size_t start = 2;
            for (;;)
            {
                const std::size_t tab = line.find('\t', start);
                fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            return fields;
        }
    }

    JavaScriptRuntime::JavaScriptRuntime()
    {
        _path = settingsPath().parent_path() / "visualizer.javascript.ini";
        load();
    }

    JavaScriptRuntime::~JavaScriptRuntime()
    {
        runtimeReloadAllWorkspaceScripts();
        save();
    }

    RuntimeScript& JavaScriptRuntime::add()
    {
        _scripts.emplace_back();
        auto& script = _scripts.back();
        script.Id = _nextScriptId++;
        script.Order = static_cast<int>(_scripts.size() - 1);
        std::snprintf(script.Name, sizeof(script.Name), "JavaScript %zu", _scripts.size());
        ++_revision;
        return script;
    }

    void JavaScriptRuntime::erase(const std::size_t index, RuntimeBindingEngine& runtime)
    {
        if (index >= _scripts.size()) return;
        const std::uint64_t id = _scripts[index].Id;
        runtimeResetWorkspaceScript(id);
        _scriptOutputs.erase(id);
        _scripts.erase(_scripts.begin() + static_cast<std::ptrdiff_t>(index));
        for (auto& profile : runtime.profiles()) std::erase(profile.ScriptIds, id);
        rebuildOutput();
        runtime.markChanged();
        ++_revision;
    }

    RuntimeScript* JavaScriptRuntime::find(const std::uint64_t id) noexcept
    {
        const auto it = std::ranges::find(_scripts, id, &RuntimeScript::Id);
        return it == _scripts.end() ? nullptr : &*it;
    }

    const RuntimeScript* JavaScriptRuntime::find(const std::uint64_t id) const noexcept
    {
        const auto it = std::ranges::find(_scripts, id, &RuntimeScript::Id);
        return it == _scripts.end() ? nullptr : &*it;
    }

    RuntimeScript* JavaScriptRuntime::findByName(const std::string_view name) noexcept
    {
        const auto it = std::ranges::find_if(_scripts, [&](const RuntimeScript& script) { return std::string_view(script.Name) == name; });
        return it == _scripts.end() ? nullptr : &*it;
    }

    const RuntimeScript* JavaScriptRuntime::findByName(const std::string_view name) const noexcept
    {
        const auto it = std::ranges::find_if(_scripts, [&](const RuntimeScript& script) { return std::string_view(script.Name) == name; });
        return it == _scripts.end() ? nullptr : &*it;
    }

    void JavaScriptRuntime::clearOutput(const std::uint64_t scriptId) noexcept
    {
        if (_scriptOutputs.erase(scriptId) != 0) rebuildOutput();
    }

    void JavaScriptRuntime::clearOutputs() noexcept
    {
        _scriptOutputs.clear();
        _output = {};
    }

    void JavaScriptRuntime::rebuildOutput() noexcept
    {
        RuntimeControlOutput combined{};
        std::vector<const RuntimeScript*> order;
        order.reserve(_scripts.size());
        for (const auto& script : _scripts) if (script.Enabled) order.push_back(&script);
        std::ranges::stable_sort(order, [](const RuntimeScript* a, const RuntimeScript* b) { if (a->Order != b->Order) return a->Order < b->Order; return a->Id < b->Id; });
        for (const RuntimeScript* script : order)
        {
            const auto it = _scriptOutputs.find(script->Id);
            if (it == _scriptOutputs.end()) continue;
            const auto& source = it->second;
            if (source.ShaderId) { combined.ShaderId = source.ShaderId; combined.ShaderPresetIndex.reset(); combined.ShaderTransitionSeconds = source.ShaderTransitionSeconds; }
            if (source.ShaderPresetIndex) { combined.ShaderPresetIndex = source.ShaderPresetIndex; combined.ShaderId.reset(); combined.ShaderTransitionSeconds = source.ShaderTransitionSeconds; }
            if (source.GlobalBrightness) combined.GlobalBrightness = source.GlobalBrightness;
            if (source.SendFramebuffer) combined.SendFramebuffer = source.SendFramebuffer;
            if (source.BaseColorMode) combined.BaseColorMode = source.BaseColorMode;
        }
        _output = std::move(combined);
    }

    bool JavaScriptRuntime::save()
    {
        std::error_code ec;
        std::filesystem::create_directories(_path.parent_path(), ec);
        const auto temporary = std::filesystem::path(_path.string() + ".tmp");
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return false;
        file << "# Quartz script runtime v2\n";
        file << "J\t" << _settings.ExternalHotReload << '\t' << _settings.ReloadHotkeyCtrl << '\t' << _settings.ReloadHotkeyAlt << '\t' << _settings.ReloadHotkeyShift << '\t' << _settings.ReloadHotkeyKey << '\n';
        for (const auto& script : _scripts)
            file << "S\t" << script.Enabled << '\t' << script.Id << '\t' << runtimeEscape(script.Name) << '\t' << script.External << '\t' << runtimeEscape(script.Path) << '\t' << script.HotReload << '\t' << script.UpdateHz << '\t' << script.TimeoutMs << '\t' << script.Order << '\t' << runtimeEscape(script.Group) << '\t' << runtimeEscape(script.PersistentStateJson) << '\t' << runtimeEscape(script.Source) << '\n';
        file.close();
        if (!file) return false;
        std::filesystem::rename(temporary, _path, ec);
        if (ec)
        {
            std::filesystem::remove(_path, ec);
            ec.clear();
            std::filesystem::rename(temporary, _path, ec);
        }
        if (!ec) _savedRevision = _revision;
        return !ec;
    }

    void JavaScriptRuntime::load()
    {
        std::ifstream file(_path);
        if (!file) return;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.starts_with("J\t"))
            {
                const auto fields = fieldsOf(line);
                if (!fields.empty()) parseBool(fields[0], _settings.ExternalHotReload);
                if (fields.size() > 1) parseBool(fields[1], _settings.ReloadHotkeyCtrl);
                if (fields.size() > 2) parseBool(fields[2], _settings.ReloadHotkeyAlt);
                if (fields.size() > 3) parseBool(fields[3], _settings.ReloadHotkeyShift);
                if (fields.size() > 4) parseNumber(fields[4], _settings.ReloadHotkeyKey);
            }
            else if (line.starts_with("S\t"))
            {
                const auto fields = fieldsOf(line);
                if (fields.size() < 10) continue;
                RuntimeScript script;
                parseBool(fields[0], script.Enabled);
                parseNumber(fields[1], script.Id);
                copyField(script.Name, runtimeUnescape(fields[2]));
                parseBool(fields[3], script.External);
                script.Path = runtimeUnescape(fields[4]);
                parseBool(fields[5], script.HotReload);
                parseNumber(fields[6], script.UpdateHz);
                parseNumber(fields[7], script.TimeoutMs);
                parseNumber(fields[8], script.Order);
                copyField(script.Group, runtimeUnescape(fields[9]));
                const bool oldLayout = fields.size() > 12;
                const std::size_t storageIndex = oldLayout ? 11 : 10;
                const std::size_t sourceIndex = oldLayout ? 12 : 11;
                if (fields.size() > storageIndex) script.PersistentStateJson = runtimeUnescape(fields[storageIndex]);
                if (fields.size() > sourceIndex) script.Source = runtimeUnescape(fields[sourceIndex]);
                script.UpdateHz = std::clamp(script.UpdateHz, 0.5f, 500.0f);
                script.TimeoutMs = std::clamp(script.TimeoutMs, 0.1f, 100.0f);
                if (script.PersistentStateJson.empty()) script.PersistentStateJson = "{}";
                if (script.Id == 0) script.Id = _nextScriptId++;
                else _nextScriptId = std::max(_nextScriptId, script.Id + 1);
                _scripts.emplace_back(std::move(script));
            }
        }
        _savedRevision = _revision;
    }

    void JavaScriptRuntime::syncProfile(RuntimeBindingEngine& runtime)
    {
        const std::uint64_t activeId = runtime.activeProfileId();
        const std::uint64_t profileRevision = runtime.revision();
        if (_observedProfileId == activeId && _observedProfileRevision == profileRevision) return;
        _observedProfileId = activeId;
        _observedProfileRevision = profileRevision;
        if (activeId == 0) return;
        const RuntimeBindingProfile* profile = runtime.findProfile(activeId);
        if (!profile) return;
        bool changed = false;
        for (auto& script : _scripts)
        {
            const bool enabled = std::find(profile->ScriptIds.begin(), profile->ScriptIds.end(), script.Id) != profile->ScriptIds.end();
            if (script.Enabled == enabled) continue;
            script.Enabled = enabled;
            if (!enabled) { runtimeResetWorkspaceScript(script.Id); _scriptOutputs.erase(script.Id); }
            changed = true;
        }
        if (changed) { rebuildOutput(); ++_revision; }
    }

    void JavaScriptRuntime::pollReloadHotkey(GLFWwindow* window, const EvdevKeyboard& keyboard)
    {
        if (_settings.ReloadHotkeyKey <= 0) { _settings.ReloadHotkeyDown = false; return; }
        bool down = false;
        if (keyboard.connected())
        {
            const std::uint16_t key = evdevKeyFromGlfw(_settings.ReloadHotkeyKey);
            down = key != 0 && keyboard.shortcutDown(key, _settings.ReloadHotkeyCtrl, _settings.ReloadHotkeyAlt, _settings.ReloadHotkeyShift);
        }
        else if (window)
        {
            const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            const bool alt = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
            const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            down = (!_settings.ReloadHotkeyCtrl || ctrl) && (!_settings.ReloadHotkeyAlt || alt) && (!_settings.ReloadHotkeyShift || shift) && glfwGetKey(window, _settings.ReloadHotkeyKey) == GLFW_PRESS;
        }
        if (down && !_settings.ReloadHotkeyDown) { runtimeReloadAllWorkspaceScripts(); clearOutputs(); }
        _settings.ReloadHotkeyDown = down;
    }
}
