#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def read(path): return (ROOT / path).read_text()
def write(path, text):
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1: raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

# --- Runtime model: first-class scripts + profile membership ---
p = "include/quartz/client/runtime/Profile.hpp"
text = read(p)
text = replace_once(text, "        std::vector<std::uint64_t> ControlIds;\n        bool HotkeyCtrl = false;", "        std::vector<std::uint64_t> ControlIds;\n        std::vector<std::uint64_t> ScriptIds;\n        bool HotkeyCtrl = false;", "profile script ids")
write(p, text)

p = "include/quartz/client/runtime/RuntimeTypes.hpp"
text = read(p)
anchor = "    struct RuntimeBinding\n    {\n"
insert = r'''    struct RuntimeScript
    {
        std::uint64_t Id = 0;
        bool Enabled = true;
        int Order = 0;
        char Group[64]{};
        char Name[64] = "JavaScript";
        bool External = false;
        std::string Path;
        std::string Source = "// Quartz runtime script\n";
        bool HotReload = true;
        float UpdateHz = 60.0f;
        float TimeoutMs = 4.0f;
        double NextUpdate = 0.0;
        double LastMilliseconds = 0.0;
        std::uint64_t RunCount = 0;
        std::uint64_t CompileCount = 0;
        std::uint64_t ReloadCount = 0;
        std::uint64_t TimeoutCount = 0;
        std::string LastLog;
        std::string Status;
        std::vector<std::string> Dependencies;
    };

    struct RuntimeScriptSettings
    {
        bool ExternalHotReload = true;
        bool ReloadHotkeyCtrl = false;
        bool ReloadHotkeyAlt = false;
        bool ReloadHotkeyShift = false;
        int ReloadHotkeyKey = 0;
        bool ReloadHotkeyDown = false;
    };

'''
text = replace_once(text, anchor, insert + anchor, "runtime script structs")
write(p, text)

# --- QuickJS shared context/API declarations ---
p = "src/runtime/QuickJSInternal.hpp"
text = read(p)
text = replace_once(text, "    struct RuntimeBinding;\n    struct RuntimeSignalContext;", "    struct RuntimeBinding;\n    struct RuntimeScript;\n    struct RuntimeSignalContext;\n    struct RuntimeControlOutput;\n    class ShaderFramebuffer;", "quickjs forward decls")
text = replace_once(text, "        RuntimeBinding* Binding = nullptr;\n        const RuntimeSignalContext* SignalContext = nullptr;\n        RuntimeQuickJSDeadline* Execution = nullptr;", "        RuntimeBinding* Binding = nullptr;\n        RuntimeScript* Script = nullptr;\n        const RuntimeSignalContext* SignalContext = nullptr;\n        RuntimeQuickJSDeadline* Execution = nullptr;\n        ShaderFramebuffer* Shader = nullptr;\n        RuntimeControlOutput* Output = nullptr;\n        bool AllowGraphMutation = false;", "quickjs context workspace fields")
text = replace_once(text, "    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);", "    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api);\n    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api);", "graph api decl")
write(p, text)

p = "include/quartz/client/runtime/QuickJS.hpp"
text = read(p)
text = replace_once(text, "    struct RuntimeBinding;\n    struct RuntimeSignalContext;", "    struct RuntimeBinding;\n    struct RuntimeScript;\n    struct RuntimeSignalContext;\n    struct RuntimeControlOutput;\n    class ShaderFramebuffer;", "quickjs public forward decls")
text = replace_once(text, "    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);\n", "    bool runtimeSaveQuickJSTypeDeclarations(std::string& error);\n    RuntimeControlOutput runtimeEvaluateWorkspaceScripts(RuntimeBindingEngine& engine, const RuntimeSignalContext& context, ShaderFramebuffer& shader);\n    void runtimeResetWorkspaceScript(std::uint64_t scriptId) noexcept;\n    void runtimeReloadAllWorkspaceScripts() noexcept;\n    std::filesystem::path runtimeQuickJSScriptDirectory();\n    std::string_view runtimeTerrariaAstrofluxMigrationScript() noexcept;\n    bool runtimeInstallTerrariaAstrofluxMigration(RuntimeBindingEngine& engine, std::string& error);\n")
write(p, text)

# --- Engine persistence/ownership/profile integration ---
p = "include/quartz/client/runtime/RuntimeBindingEngine.hpp"
text = read(p)
text = replace_once(text, "        ~RuntimeBindingEngine() { for (const auto& binding : _bindings) if (binding.Source == RuntimeSourceKind::Script) runtimeResetScriptBinding(binding.Id); save(); }", "        ~RuntimeBindingEngine() { for (const auto& binding : _bindings) if (binding.Source == RuntimeSourceKind::Script) runtimeResetScriptBinding(binding.Id); for (const auto& script : _scripts) runtimeResetWorkspaceScript(script.Id); save(); }", "engine destructor scripts")
text = replace_once(text, "        std::vector<RuntimeBindingProfile>& profiles() noexcept { return _profiles; }\n        const std::vector<RuntimeBindingProfile>& profiles() const noexcept { return _profiles; }", "        std::vector<RuntimeBindingProfile>& profiles() noexcept { return _profiles; }\n        const std::vector<RuntimeBindingProfile>& profiles() const noexcept { return _profiles; }\n        std::vector<RuntimeScript>& scripts() noexcept { return _scripts; }\n        const std::vector<RuntimeScript>& scripts() const noexcept { return _scripts; }\n        RuntimeScriptSettings& scriptSettings() noexcept { return _scriptSettings; }\n        const RuntimeScriptSettings& scriptSettings() const noexcept { return _scriptSettings; }", "engine script accessors")
add_profile_anchor = '''        RuntimeBindingProfile& addProfile()\n        {\n            _profiles.emplace_back();'''.replace('\\n','\n')
script_methods = r'''        RuntimeScript& addScript()
        {
            _scripts.emplace_back();
            auto& script = _scripts.back();
            script.Id = _nextScriptId++;
            script.Order = static_cast<int>(_scripts.size() - 1);
            std::snprintf(script.Name, sizeof(script.Name), "JavaScript %zu", _scripts.size());
            ++_revision;
            return script;
        }

        void eraseScript(const std::size_t index)
        {
            if (index >= _scripts.size()) return;
            const std::uint64_t id = _scripts[index].Id;
            runtimeResetWorkspaceScript(id);
            _scripts.erase(_scripts.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& profile : _profiles) std::erase(profile.ScriptIds, id);
            ++_revision;
        }

        RuntimeScript* findScript(const std::uint64_t id) noexcept { ensureRuntimeCaches(); const auto it = _scriptLookup.find(id); return it == _scriptLookup.end() ? nullptr : &_scripts[it->second]; }
        const RuntimeScript* findScript(const std::uint64_t id) const noexcept { ensureRuntimeCaches(); const auto it = _scriptLookup.find(id); return it == _scriptLookup.end() ? nullptr : &_scripts[it->second]; }
        RuntimeScript* findScriptByName(const std::string_view name) noexcept { ensureRuntimeCaches(); const auto it = _scriptNameLookup.find(name); return it == _scriptNameLookup.end() ? nullptr : &_scripts[it->second]; }
        const RuntimeScript* findScriptByName(const std::string_view name) const noexcept { ensureRuntimeCaches(); const auto it = _scriptNameLookup.find(name); return it == _scriptNameLookup.end() ? nullptr : &_scripts[it->second]; }

'''
text = replace_once(text, add_profile_anchor, script_methods + add_profile_anchor, "engine script methods")
text = replace_once(text, "            for (const auto id : profile.ControlIds) if (auto* control = findControl(id)) control->Enabled = enabled;\n            ++_revision;", "            for (const auto id : profile.ControlIds) if (auto* control = findControl(id)) control->Enabled = enabled;\n            for (const auto id : profile.ScriptIds) if (auto* script = findScript(id)) script->Enabled = enabled;\n            ++_revision;", "profile members scripts")
text = replace_once(text, "                for (auto& control : _controls) control.Enabled = false;\n            }", "                for (auto& control : _controls) control.Enabled = false;\n                for (auto& script : _scripts) script.Enabled = false;\n            }", "exclusive profile scripts")
# Extend evdev key translation for numeric hotkeys.
text = replace_once(text, "                case GLFW_KEY_V: return KEY_V; case GLFW_KEY_W: return KEY_W; case GLFW_KEY_X: return KEY_X; case GLFW_KEY_Y: return KEY_Y; case GLFW_KEY_Z: return KEY_Z;\n                default: return 0;", "                case GLFW_KEY_V: return KEY_V; case GLFW_KEY_W: return KEY_W; case GLFW_KEY_X: return KEY_X; case GLFW_KEY_Y: return KEY_Y; case GLFW_KEY_Z: return KEY_Z;\n                case GLFW_KEY_0: return KEY_0; case GLFW_KEY_1: return KEY_1; case GLFW_KEY_2: return KEY_2; case GLFW_KEY_3: return KEY_3; case GLFW_KEY_4: return KEY_4; case GLFW_KEY_5: return KEY_5; case GLFW_KEY_6: return KEY_6; case GLFW_KEY_7: return KEY_7; case GLFW_KEY_8: return KEY_8; case GLFW_KEY_9: return KEY_9;\n                default: return 0;", "profile numeric evdev")
# Add global external-script reload hotkey method after profile polling.
profile_end = '''                profile.HotkeyDown = down;\n            }\n        }\n\n        RuntimeObjectPointer& addPointer()'''.replace('\\n','\n')
reload_method = r'''                profile.HotkeyDown = down;
            }
        }

        void pollScriptReloadHotkey(GLFWwindow* window, const EvdevKeyboard& keyboard)
        {
            auto& settings = _scriptSettings;
            if (settings.ReloadHotkeyKey <= 0) { settings.ReloadHotkeyDown = false; return; }
            const auto evdevKey = [](const int key) -> std::uint16_t
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
            };
            bool down = false;
            if (keyboard.connected())
            {
                const std::uint16_t key = evdevKey(settings.ReloadHotkeyKey); down = key != 0 && keyboard.shortcutDown(key, settings.ReloadHotkeyCtrl, settings.ReloadHotkeyAlt, settings.ReloadHotkeyShift);
            }
            else if (window)
            {
                const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
                const bool alt = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
                const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                down = (!settings.ReloadHotkeyCtrl || ctrl) && (!settings.ReloadHotkeyAlt || alt) && (!settings.ReloadHotkeyShift || shift) && glfwGetKey(window, settings.ReloadHotkeyKey) == GLFW_PRESS;
            }
            if (down && !settings.ReloadHotkeyDown) runtimeReloadAllWorkspaceScripts();
            settings.ReloadHotkeyDown = down;
        }

        RuntimeObjectPointer& addPointer()'''
text = replace_once(text, profile_end, reload_method, "script reload hotkey")
# Public binding operation bridge for q.graph.
text = replace_once(text, "        void markChanged() noexcept { ++_revision; }", "        void markChanged() noexcept { ++_revision; }\n        bool operateBinding(RuntimeBinding& binding, const RuntimeBindingOperation operation) { applyBindingOperation(binding, operation); ++_revision; return true; }", "binding operation bridge")
# Save v12 + scripts/settings/profile memberships.
text = text.replace('file << "# Quartz runtime material bindings v11\\n";', 'file << "# Quartz runtime material bindings v12\\n";', 1)
text = replace_once(text, "            for (const auto& profile : _profiles)\n                file << \"P\\t\" << profile.Enabled << '\\t' << profile.Id << '\\t' << runtimeEscape(profile.Name) << '\\t' << profile.Exclusive << '\\t'\n                     << profile.HotkeyCtrl << '\\t' << profile.HotkeyAlt << '\\t' << profile.HotkeyShift << '\\t' << profile.HotkeyKey << '\\t'\n                     << serializeIdList(profile.BindingIds) << '\\t' << serializeIdList(profile.ControlIds) << '\\n';", "            file << \"J\\t\" << _scriptSettings.ExternalHotReload << '\\t' << _scriptSettings.ReloadHotkeyCtrl << '\\t' << _scriptSettings.ReloadHotkeyAlt << '\\t' << _scriptSettings.ReloadHotkeyShift << '\\t' << _scriptSettings.ReloadHotkeyKey << '\\n';\n            for (const auto& script : _scripts)\n                file << \"S\\t\" << script.Enabled << '\\t' << script.Id << '\\t' << runtimeEscape(script.Name) << '\\t' << script.External << '\\t' << runtimeEscape(script.Path) << '\\t' << script.HotReload << '\\t' << script.UpdateHz << '\\t' << script.TimeoutMs << '\\t' << script.Order << '\\t' << runtimeEscape(script.Group) << '\\t' << runtimeEscape(script.Source) << '\\n';\n            for (const auto& profile : _profiles)\n                file << \"P\\t\" << profile.Enabled << '\\t' << profile.Id << '\\t' << runtimeEscape(profile.Name) << '\\t' << profile.Exclusive << '\\t'\n                     << profile.HotkeyCtrl << '\\t' << profile.HotkeyAlt << '\\t' << profile.HotkeyShift << '\\t' << profile.HotkeyKey << '\\t'\n                     << serializeIdList(profile.BindingIds) << '\\t' << serializeIdList(profile.ControlIds) << '\\t' << serializeIdList(profile.ScriptIds) << '\\n';", "save scripts profiles")
# Load S/J and extended P.
p_load_anchor = '''                else if (line.starts_with("P\\t"))\n                {'''.replace('\\n','\n')
load_scripts = r'''                else if (line.starts_with("J\t"))
                {
                    std::vector<std::string> fields; std::size_t start = 2; for (;;) { const std::size_t tab = line.find('\t', start); fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)); if (tab == std::string::npos) break; start = tab + 1; }
                    if (!fields.empty()) parseBool(fields[0], _scriptSettings.ExternalHotReload); if (fields.size() > 1) parseBool(fields[1], _scriptSettings.ReloadHotkeyCtrl); if (fields.size() > 2) parseBool(fields[2], _scriptSettings.ReloadHotkeyAlt); if (fields.size() > 3) parseBool(fields[3], _scriptSettings.ReloadHotkeyShift); if (fields.size() > 4) parseNumber(fields[4], _scriptSettings.ReloadHotkeyKey);
                }
                else if (line.starts_with("S\t"))
                {
                    std::vector<std::string> fields; std::size_t start = 2; for (;;) { const std::size_t tab = line.find('\t', start); fields.emplace_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)); if (tab == std::string::npos) break; start = tab + 1; }
                    if (fields.size() < 10) continue; RuntimeScript script; parseBool(fields[0], script.Enabled); parseNumber(fields[1], script.Id); copyField(script.Name, runtimeUnescape(fields[2])); parseBool(fields[3], script.External); script.Path = runtimeUnescape(fields[4]); parseBool(fields[5], script.HotReload); parseNumber(fields[6], script.UpdateHz); parseNumber(fields[7], script.TimeoutMs); parseNumber(fields[8], script.Order); copyField(script.Group, runtimeUnescape(fields[9])); if (fields.size() > 10) script.Source = runtimeUnescape(fields[10]); script.UpdateHz = std::clamp(script.UpdateHz, 0.5f, 500.0f); script.TimeoutMs = std::clamp(script.TimeoutMs, 0.1f, 100.0f); if (script.Id == 0) script.Id = _nextScriptId++; else _nextScriptId = std::max(_nextScriptId, script.Id + 1); _scripts.emplace_back(std::move(script));
                }
                else if (line.starts_with("P\t"))
                {'''
text = replace_once(text, p_load_anchor, load_scripts, "load script records")
text = replace_once(text, "                    parseIdList(fields[8], profile.BindingIds); parseIdList(fields[9], profile.ControlIds);", "                    parseIdList(fields[8], profile.BindingIds); parseIdList(fields[9], profile.ControlIds); if (fields.size() > 10) parseIdList(fields[10], profile.ScriptIds);", "load profile scripts")
# Cache script IDs/names.
text = replace_once(text, "            _bindingLookup.clear(); _controlLookup.clear(); _bankLookup.clear(); _profileLookup.clear(); _objectLookup.clear(); _pointerLookup.clear();\n            _bindingNameLookup.clear(); _controlNameLookup.clear(); _bankNameLookup.clear();", "            _bindingLookup.clear(); _controlLookup.clear(); _bankLookup.clear(); _profileLookup.clear(); _objectLookup.clear(); _pointerLookup.clear(); _scriptLookup.clear();\n            _bindingNameLookup.clear(); _controlNameLookup.clear(); _bankNameLookup.clear(); _scriptNameLookup.clear();", "clear script caches")
text = replace_once(text, "            _profileLookup.reserve(_profiles.size()); for (std::size_t i = 0; i < _profiles.size(); ++i) _profileLookup.emplace(_profiles[i].Id, i);", "            _profileLookup.reserve(_profiles.size()); for (std::size_t i = 0; i < _profiles.size(); ++i) _profileLookup.emplace(_profiles[i].Id, i);\n            _scriptLookup.reserve(_scripts.size()); _scriptNameLookup.reserve(_scripts.size()); for (std::size_t i = 0; i < _scripts.size(); ++i) { _scriptLookup.emplace(_scripts[i].Id, i); if (_scripts[i].Name[0]) _scriptNameLookup.try_emplace(_scripts[i].Name, i); }", "build script caches")
# Members.
text = replace_once(text, "        std::vector<RuntimeBindingProfile> _profiles;\n        std::uint64_t _nextBindingId = 1;", "        std::vector<RuntimeBindingProfile> _profiles;\n        std::vector<RuntimeScript> _scripts;\n        RuntimeScriptSettings _scriptSettings{};\n        std::uint64_t _nextBindingId = 1;", "engine script members")
text = replace_once(text, "        std::uint64_t _nextProfileId = 1;\n        std::uint64_t _activeProfileId = 0;", "        std::uint64_t _nextProfileId = 1;\n        std::uint64_t _nextScriptId = 1;\n        std::uint64_t _activeProfileId = 0;", "next script id")
text = replace_once(text, "        mutable std::unordered_map<std::uint64_t, std::size_t> _bindingLookup, _controlLookup, _bankLookup, _profileLookup, _objectLookup, _pointerLookup;\n        mutable StringLookup _bindingNameLookup, _controlNameLookup, _bankNameLookup;", "        mutable std::unordered_map<std::uint64_t, std::size_t> _bindingLookup, _controlLookup, _bankLookup, _profileLookup, _objectLookup, _pointerLookup, _scriptLookup;\n        mutable StringLookup _bindingNameLookup, _controlNameLookup, _bankNameLookup, _scriptNameLookup;", "script lookup members")
write(p, text)

# --- Full graph/runtime mutation API ---
write("src/runtime/QuickJSGraphApi.cpp", r'''#include "QuickJSInternal.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include <quickjs.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::client
{
    namespace
    {
        RuntimeQuickJSContext* state(JSContext* ctx) noexcept { return static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); }
        JSValue mutationError(JSContext* ctx) { return JS_ThrowTypeError(ctx, "q.graph mutation is only available to JavaScript workspace scripts"); }
        bool mutationAllowed(JSContext* ctx) { const auto* s = state(ctx); return s && s->AllowGraphMutation && s->Engine; }
        std::string normalized(std::string value) { std::erase_if(value, [](const unsigned char c) { return c == ' ' || c == '_' || c == '-' || c == ':' || c == '.'; }); std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
        bool toString(JSContext* ctx, JSValueConst value, std::string& out) { const char* raw = JS_ToCString(ctx, value); if (!raw) return false; out = raw; JS_FreeCString(ctx, raw); return true; }
        bool property(JSContext* ctx, JSValueConst object, const char* name, JSValue& value) { value = JS_GetPropertyStr(ctx, object, name); return !JS_IsUndefined(value); }
        bool stringProperty(JSContext* ctx, JSValueConst object, const char* name, std::string& value) { JSValue p; if (!property(ctx, object, name, p)) return false; const bool ok = toString(ctx, p, value); JS_FreeValue(ctx, p); return ok; }
        bool boolProperty(JSContext* ctx, JSValueConst object, const char* name, bool& value) { JSValue p; if (!property(ctx, object, name, p)) return false; const int v = JS_ToBool(ctx, p); JS_FreeValue(ctx, p); if (v < 0) return false; value = v != 0; return true; }
        bool intProperty(JSContext* ctx, JSValueConst object, const char* name, int& value) { JSValue p; if (!property(ctx, object, name, p)) return false; std::int32_t v = 0; const int ok = JS_ToInt32(ctx, &v, p); JS_FreeValue(ctx, p); if (ok < 0) return false; value = v; return true; }
        bool u64Value(JSContext* ctx, JSValueConst value, std::uint64_t& out)
        {
            if (JS_IsBigInt(ctx, value)) { std::int64_t v = 0; if (JS_ToInt64Ext(ctx, &v, value) < 0 || v < 0) return false; out = static_cast<std::uint64_t>(v); return true; }
            std::int64_t v = 0; if (JS_ToInt64Ext(ctx, &v, value) < 0 || v < 0) return false; out = static_cast<std::uint64_t>(v); return true;
        }
        bool floatProperty(JSContext* ctx, JSValueConst object, const char* name, float& value) { JSValue p; if (!property(ctx, object, name, p)) return false; double v = 0; const int ok = JS_ToFloat64(ctx, &v, p); JS_FreeValue(ctx, p); if (ok < 0 || !std::isfinite(v)) return false; value = static_cast<float>(v); return true; }
        template<std::size_t N> void copy(char (&dst)[N], const std::string& value) { std::snprintf(dst, N, "%s", value.c_str()); }
        std::uint32_t arrayLength(JSContext* ctx, JSValueConst value) { JSValue length = JS_GetPropertyStr(ctx, value, "length"); std::uint32_t result = 0; if (JS_ToUint32(ctx, &result, length) < 0) result = 0; JS_FreeValue(ctx, length); return result; }

        template<typename E> bool enumValue(JSContext* ctx, JSValueConst value, const std::initializer_list<std::pair<std::string_view, E>> names, E& out)
        {
            if (JS_IsNumber(value)) { std::int32_t raw = 0; if (JS_ToInt32(ctx, &raw, value) < 0) return false; out = static_cast<E>(raw); return true; }
            std::string text; if (!toString(ctx, value, text)) return false; text = normalized(std::move(text));
            for (const auto& [name, candidate] : names) if (text == normalized(std::string(name))) { out = candidate; return true; }
            return false;
        }
        template<typename E> bool enumProperty(JSContext* ctx, JSValueConst object, const char* name, const std::initializer_list<std::pair<std::string_view, E>> names, E& out) { JSValue p; if (!property(ctx, object, name, p)) return false; const bool ok = enumValue(ctx, p, names, out); JS_FreeValue(ctx, p); return ok; }

#define SRC_NAMES {{"Constant",RuntimeSourceKind::Constant},{"Time",RuntimeSourceKind::Time},{"Audio",RuntimeSourceKind::Audio},{"Media",RuntimeSourceKind::Media},{"Keyboard",RuntimeSourceKind::Keyboard},{"RPC",RuntimeSourceKind::RPC},{"Host",RuntimeSourceKind::Host},{"USB",RuntimeSourceKind::USB},{"RGB",RuntimeSourceKind::RGB},{"NativeProcess",RuntimeSourceKind::NativeProcess},{"BindingStatus",RuntimeSourceKind::BindingStatus},{"Unbound",RuntimeSourceKind::Unbound},{"BindingValue",RuntimeSourceKind::BindingValue},{"ShaderState",RuntimeSourceKind::ShaderState},{"ControlStatus",RuntimeSourceKind::ControlStatus},{"Aggregate",RuntimeSourceKind::Aggregate},{"MassCompare",RuntimeSourceKind::MassCompare},{"NativeAddress",RuntimeSourceKind::NativeAddress},{"ObjectField",RuntimeSourceKind::ObjectField},{"ObjectStatus",RuntimeSourceKind::ObjectStatus},{"ValueBank",RuntimeSourceKind::ValueBank},{"StringConstant",RuntimeSourceKind::StringConstant},{"ProfileState",RuntimeSourceKind::ProfileState},{"Script",RuntimeSourceKind::Script}}
#define PV_NAMES {{"u8",ProcessValueType::U8},{"i8",ProcessValueType::I8},{"u16",ProcessValueType::U16},{"i16",ProcessValueType::I16},{"u32",ProcessValueType::U32},{"i32",ProcessValueType::I32},{"u64",ProcessValueType::U64},{"i64",ProcessValueType::I64},{"float",ProcessValueType::Float},{"double",ProcessValueType::Double},{"bool",ProcessValueType::Bool}}
#define REBIND_NAMES {{"NameExact",ProcessRebindMode::NameExact},{"ExecutableExact",ProcessRebindMode::ExecutableExact},{"TitleExact",ProcessRebindMode::TitleExact},{"CommandLineExact",ProcessRebindMode::CommandLineExact},{"NameRegex",ProcessRebindMode::NameRegex},{"ExecutableRegex",ProcessRebindMode::ExecutableRegex},{"TitleRegex",ProcessRebindMode::TitleRegex},{"CommandLineRegex",ProcessRebindMode::CommandLineRegex},{"AnyRegex",ProcessRebindMode::AnyRegex}}
#define SIGRES_NAMES {{"MatchAddress",SignatureResultMode::MatchAddress},{"RipRelative32",SignatureResultMode::RipRelative32},{"PointerAtOffset",SignatureResultMode::PointerAtOffset},{"RegisterRelativeCapture",SignatureResultMode::RegisterRelativeCapture},{"Address32AtOffset",SignatureResultMode::Address32AtOffset}}
#define CMP_NAMES {{"Equal",RuntimeCompareCondition::Equal},{"NotEqual",RuntimeCompareCondition::NotEqual},{"Less",RuntimeCompareCondition::Less},{"LessEqual",RuntimeCompareCondition::LessEqual},{"Greater",RuntimeCompareCondition::Greater},{"GreaterEqual",RuntimeCompareCondition::GreaterEqual},{"Between",RuntimeCompareCondition::Between},{"Outside",RuntimeCompareCondition::Outside}}
#define CTRL_COND_NAMES {{"Equal",RuntimeControlCondition::Equal},{"NotEqual",RuntimeControlCondition::NotEqual},{"Less",RuntimeControlCondition::Less},{"LessEqual",RuntimeControlCondition::LessEqual},{"Greater",RuntimeControlCondition::Greater},{"GreaterEqual",RuntimeControlCondition::GreaterEqual},{"Between",RuntimeControlCondition::Between},{"Outside",RuntimeControlCondition::Outside},{"RisingEdge",RuntimeControlCondition::RisingEdge},{"FallingEdge",RuntimeControlCondition::FallingEdge},{"OnChange",RuntimeControlCondition::OnChange},{"ChangedTo",RuntimeControlCondition::ChangedTo},{"ChangedFrom",RuntimeControlCondition::ChangedFrom},{"BecomesTrue",RuntimeControlCondition::BecomesTrue},{"BecomesFalse",RuntimeControlCondition::BecomesFalse},{"StringEqual",RuntimeControlCondition::StringEqual},{"StringNotEqual",RuntimeControlCondition::StringNotEqual},{"StringContains",RuntimeControlCondition::StringContains}}
#define CTRL_TARGET_NAMES {{"ActiveShader",RuntimeControlTarget::ActiveShader},{"BindingEnabled",RuntimeControlTarget::BindingEnabled},{"GlobalBrightness",RuntimeControlTarget::GlobalBrightness},{"SendFramebuffer",RuntimeControlTarget::SendFramebuffer},{"BaseColorMode",RuntimeControlTarget::BaseColorMode},{"MaterialParameter",RuntimeControlTarget::MaterialParameter},{"BindingValue",RuntimeControlTarget::BindingValue},{"ValueBank",RuntimeControlTarget::ValueBank},{"ControlEnabled",RuntimeControlTarget::ControlEnabled},{"BindingRefresh",RuntimeControlTarget::BindingRefresh},{"BindingForceUpdate",RuntimeControlTarget::BindingForceUpdate},{"BindingInvalidate",RuntimeControlTarget::BindingInvalidate},{"BindingResetState",RuntimeControlTarget::BindingResetState},{"BindingRetryRegisterCapture",RuntimeControlTarget::BindingRetryRegisterCapture},{"BindingRescanPattern",RuntimeControlTarget::BindingRescanPattern},{"BindingRebindProcess",RuntimeControlTarget::BindingRebindProcess},{"BindingClearError",RuntimeControlTarget::BindingClearError}}
#define ACTION_TARGET_NAMES {{"ActiveShader",RuntimeActionTarget::ActiveShader},{"BindingEnabled",RuntimeActionTarget::BindingEnabled},{"GlobalBrightness",RuntimeActionTarget::GlobalBrightness},{"SendFramebuffer",RuntimeActionTarget::SendFramebuffer},{"BaseColorMode",RuntimeActionTarget::BaseColorMode},{"MaterialParameter",RuntimeActionTarget::MaterialParameter},{"BindingValue",RuntimeActionTarget::BindingValue},{"ValueBank",RuntimeActionTarget::ValueBank},{"ControlEnabled",RuntimeActionTarget::ControlEnabled},{"BindingRefresh",RuntimeActionTarget::BindingRefresh},{"BindingForceUpdate",RuntimeActionTarget::BindingForceUpdate},{"BindingInvalidate",RuntimeActionTarget::BindingInvalidate},{"BindingResetState",RuntimeActionTarget::BindingResetState},{"BindingRetryRegisterCapture",RuntimeActionTarget::BindingRetryRegisterCapture},{"BindingRescanPattern",RuntimeActionTarget::BindingRescanPattern},{"BindingRebindProcess",RuntimeActionTarget::BindingRebindProcess},{"BindingClearError",RuntimeActionTarget::BindingClearError}}

        RuntimeBinding* bindingRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findBinding(id); std::string name; return toString(ctx, value, name) ? engine.findBindingByName(name) : nullptr;
        }
        RuntimeControlRule* controlRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findControl(id); std::string name; return toString(ctx, value, name) ? engine.findControlByName(name) : nullptr;
        }
        RuntimeValueBankEntry* bankRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findBankValue(id); std::string name; return toString(ctx, value, name) ? engine.findBankValueByName(name) : nullptr;
        }
        RuntimeScript* scriptRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findScript(id); std::string name; return toString(ctx, value, name) ? engine.findScriptByName(name) : nullptr;
        }
        RuntimeBindingProfile* profileRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findProfile(id); std::string name; if (!toString(ctx, value, name)) return nullptr; const auto it = std::ranges::find_if(engine.profiles(), [&](const auto& profile) { return std::string_view(profile.Name) == name; }); return it == engine.profiles().end() ? nullptr : &*it;
        }
        RuntimeObjectDescriptor* objectRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findObject(id); std::string name; if (!toString(ctx, value, name)) return nullptr; const auto it = std::ranges::find_if(engine.objects(), [&](const auto& object) { return std::string_view(object.Name) == name; }); return it == engine.objects().end() ? nullptr : &*it;
        }
        RuntimeObjectPointer* pointerRef(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst value)
        {
            std::uint64_t id = 0; if ((JS_IsNumber(value) || JS_IsBigInt(ctx, value)) && u64Value(ctx, value, id)) return engine.findPointer(id); std::string name; if (!toString(ctx, value, name)) return nullptr; const auto it = std::ranges::find_if(engine.pointers(), [&](const auto& pointer) { return std::string_view(pointer.Name) == name; }); return it == engine.pointers().end() ? nullptr : &*it;
        }
        std::uint64_t bindingId(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst v) { if (auto* x = bindingRef(ctx, engine, v)) return x->Id; return 0; }
        std::uint64_t controlId(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst v) { if (auto* x = controlRef(ctx, engine, v)) return x->Id; return 0; }
        std::uint64_t bankId(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst v) { if (auto* x = bankRef(ctx, engine, v)) return x->Id; return 0; }

        void applyActions(JSContext* ctx, RuntimeBindingEngine& engine, JSValueConst array, std::vector<RuntimeAction>& actions)
        {
            actions.clear(); const auto count = arrayLength(ctx, array); actions.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                JSValue item = JS_GetPropertyUint32(ctx, array, i); if (!JS_IsObject(item)) { JS_FreeValue(ctx, item); continue; } RuntimeAction a;
                boolProperty(ctx,item,"enabled",a.Enabled); enumProperty(ctx,item,"target",ACTION_TARGET_NAMES,a.Target); enumProperty(ctx,item,"valueMode",{{"Constant",RuntimeActionValueMode::Constant},{"SourceValue",RuntimeActionValueMode::SourceValue},{"BindingValue",RuntimeActionValueMode::BindingValue},{"BankValue",RuntimeActionValueMode::BankValue}},a.ValueMode); enumProperty(ctx,item,"when",{{"WhileActive",RuntimeActionWhen::WhileActive},{"OnTrigger",RuntimeActionWhen::OnTrigger},{"OnUpdate",RuntimeActionWhen::OnUpdate},{"OnChange",RuntimeActionWhen::OnChange},{"OnTruthy",RuntimeActionWhen::OnTruthy},{"OnFalsy",RuntimeActionWhen::OnFalsy}},a.When);
                intProperty(ctx,item,"shaderPresetIndex",a.ShaderPresetIndex); std::string s; if (stringProperty(ctx,item,"shaderId",s)) copy(a.ShaderId,s); floatProperty(ctx,item,"value",a.Value); boolProperty(ctx,item,"boolValue",a.BoolValue); intProperty(ctx,item,"targetComponent",a.TargetComponent); if (stringProperty(ctx,item,"targetId",s)) copy(a.TargetId,s); if (stringProperty(ctx,item,"stringValue",s)) copy(a.StringValue,s); floatProperty(ctx,item,"transitionSeconds",a.TransitionSeconds);
                JSValue p; if (property(ctx,item,"targetBinding",p)) { a.TargetBindingId=bindingId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,item,"targetControl",p)) { a.TargetControlId=controlId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,item,"valueBinding",p)) { a.ValueBindingId=bindingId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,item,"bankValue",p)) { a.BankValueId=bankId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,item,"targetBank",p)) { a.TargetBankValueId=bankId(ctx,engine,p); JS_FreeValue(ctx,p); }
                actions.push_back(std::move(a)); JS_FreeValue(ctx,item);
            }
        }

        void applyBindingConfig(JSContext* ctx, RuntimeBindingEngine& engine, RuntimeBinding& b, JSValueConst o)
        {
            boolProperty(ctx,o,"enabled",b.Enabled); intProperty(ctx,o,"priority",b.Priority); intProperty(ctx,o,"order",b.Order); intProperty(ctx,o,"signal",b.Signal); floatProperty(ctx,o,"constant",b.Constant); std::string s; if (stringProperty(ctx,o,"group",s)) copy(b.Group,s); if (stringProperty(ctx,o,"target",s)) copy(b.TargetId,s); intProperty(ctx,o,"targetComponent",b.TargetComponent); enumProperty(ctx,o,"source",SRC_NAMES,b.Source);
            intProperty(ctx,o,"processId",b.ProcessId); boolProperty(ctx,o,"autoReattach",b.AutoReattach); enumProperty(ctx,o,"valueType",PV_NAMES,b.ValueType); if (stringProperty(ctx,o,"processName",s)) copy(b.ProcessName,s); if (stringProperty(ctx,o,"rebindPattern",s)) copy(b.ProcessRebindPattern,s); if (stringProperty(ctx,o,"module",s)) copy(b.Module,s); if (stringProperty(ctx,o,"address",s)) copy(b.Address,s); enumProperty(ctx,o,"rebindMode",REBIND_NAMES,b.RebindMode); enumProperty(ctx,o,"addressMode",{{"AddressChain",ProcessAddressMode::AddressChain},{"Signature",ProcessAddressMode::Signature}},b.AddressMode);
            if (stringProperty(ctx,o,"signature",s)) copy(b.Signature,s); boolProperty(ctx,o,"signatureExecutableOnly",b.SignatureExecutableOnly); enumProperty(ctx,o,"signatureResolve",SIGRES_NAMES,b.SignatureResolve); intProperty(ctx,o,"signatureResultOffset",b.SignatureResultOffset); intProperty(ctx,o,"signatureInstructionSize",b.SignatureInstructionSize); floatProperty(ctx,o,"signatureRetrySeconds",b.SignatureRetrySeconds); enumProperty(ctx,o,"signaturePatternKind",{{"HexadecimalPattern",RuntimeSignaturePatternKind::HexadecimalPattern},{"OpcodePattern",RuntimeSignaturePatternKind::OpcodePattern}},b.SignaturePatternKind); enumProperty(ctx,o,"signatureRegister",{{"rax",RuntimeX64Register::Rax},{"rbx",RuntimeX64Register::Rbx},{"rcx",RuntimeX64Register::Rcx},{"rdx",RuntimeX64Register::Rdx},{"rsi",RuntimeX64Register::Rsi},{"rdi",RuntimeX64Register::Rdi},{"rbp",RuntimeX64Register::Rbp},{"rsp",RuntimeX64Register::Rsp},{"r8",RuntimeX64Register::R8},{"r9",RuntimeX64Register::R9},{"r10",RuntimeX64Register::R10},{"r11",RuntimeX64Register::R11},{"r12",RuntimeX64Register::R12},{"r13",RuntimeX64Register::R13},{"r14",RuntimeX64Register::R14},{"r15",RuntimeX64Register::R15}},b.SignatureRegister); intProperty(ctx,o,"signatureRegisterDisplacementOffset",b.SignatureRegisterDisplacementOffset); enumProperty(ctx,o,"signatureDisplacementType",{{"I8",RuntimeDisplacementType::I8},{"I32",RuntimeDisplacementType::I32},{"Manual",RuntimeDisplacementType::Manual}},b.SignatureDisplacementType); intProperty(ctx,o,"signatureManualDisplacement",b.SignatureManualDisplacement); floatProperty(ctx,o,"signatureCaptureTimeoutSeconds",b.SignatureCaptureTimeoutSeconds);
            boolProperty(ctx,o,"normalize",b.Normalize); floatProperty(ctx,o,"inputMin",b.InputMin); floatProperty(ctx,o,"inputMax",b.InputMax); boolProperty(ctx,o,"invert",b.Invert); floatProperty(ctx,o,"scale",b.Scale); floatProperty(ctx,o,"offset",b.Offset); boolProperty(ctx,o,"clamp",b.Clamp); floatProperty(ctx,o,"outputMin",b.OutputMin); floatProperty(ctx,o,"outputMax",b.OutputMax); floatProperty(ctx,o,"smoothingHz",b.SmoothingHz); floatProperty(ctx,o,"updateHz",b.UpdateHz); boolProperty(ctx,o,"writeMaterial",b.WriteMaterial); floatProperty(ctx,o,"unboundValue",b.UnboundValue); if (stringProperty(ctx,o,"stringConstant",s)) copy(b.StringConstant,s); if (stringProperty(ctx,o,"script",s)) copy(b.Script,s); floatProperty(ctx,o,"scriptTimeoutMs",b.ScriptTimeoutMs); boolProperty(ctx,o,"storeToBank",b.StoreToBank);
            enumProperty(ctx,o,"aggregateOperation",{{"Sum",RuntimeAggregateOperation::Sum},{"Average",RuntimeAggregateOperation::Average},{"Minimum",RuntimeAggregateOperation::Minimum},{"Maximum",RuntimeAggregateOperation::Maximum},{"Product",RuntimeAggregateOperation::Product},{"Count",RuntimeAggregateOperation::Count},{"CountTruthy",RuntimeAggregateOperation::CountTruthy},{"FractionTruthy",RuntimeAggregateOperation::FractionTruthy},{"Any",RuntimeAggregateOperation::Any},{"All",RuntimeAggregateOperation::All}},b.AggregateOperation); enumProperty(ctx,o,"compareCondition",CMP_NAMES,b.CompareCondition); enumProperty(ctx,o,"compareResult",{{"Any",RuntimeMassCompareResult::Any},{"All",RuntimeMassCompareResult::All},{"None",RuntimeMassCompareResult::None},{"Count",RuntimeMassCompareResult::Count},{"Fraction",RuntimeMassCompareResult::Fraction},{"FirstMatchIndex",RuntimeMassCompareResult::FirstMatchIndex}},b.CompareResult); floatProperty(ctx,o,"compareA",b.CompareA); floatProperty(ctx,o,"compareB",b.CompareB); floatProperty(ctx,o,"compareTolerance",b.CompareTolerance);
            JSValue p; if (property(ctx,o,"statusBinding",p)) { b.StatusBindingId=bindingId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,o,"valueBinding",p)) { b.ValueBindingId=bindingId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,o,"controlStatus",p)) { b.ControlStatusId=controlId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,o,"bankValue",p)) { b.BankValueId=bankId(ctx,engine,p); JS_FreeValue(ctx,p); } if (property(ctx,o,"storeBankValue",p)) { b.StoreBankValueId=bankId(ctx,engine,p); JS_FreeValue(ctx,p); }
            if (property(ctx,o,"object",p)) { if (auto* x=objectRef(ctx,engine,p)) b.ObjectId=x->Id; JS_FreeValue(ctx,p); } if (property(ctx,o,"pointer",p)) { if (auto* x=pointerRef(ctx,engine,p)) b.ObjectPointerId=x->Id; JS_FreeValue(ctx,p); }
            if (property(ctx,o,"field",p)) { std::string fieldName; if (toString(ctx,p,fieldName)) if (auto* object=engine.findObject(b.ObjectId)) { const auto it=std::ranges::find_if(object->Fields,[&](const auto& f){return std::string_view(f.Name)==fieldName;}); if(it!=object->Fields.end()) b.ObjectFieldId=it->Id; } JS_FreeValue(ctx,p); }
            if (property(ctx,o,"profile",p)) { if (auto* x=profileRef(ctx,engine,p)) b.ProfileId=x->Id; JS_FreeValue(ctx,p); }
            if (property(ctx,o,"actions",p)) { applyActions(ctx,engine,p,b.Actions); JS_FreeValue(ctx,p); }
            if (property(ctx,o,"references",p))
            {
                b.References.clear(); const auto n=arrayLength(ctx,p); for(std::uint32_t i=0;i<n;++i){ JSValue r=JS_GetPropertyUint32(ctx,p,i); if(!JS_IsObject(r)){JS_FreeValue(ctx,r);continue;} RuntimeSourceReference ref; std::string kind; if(stringProperty(ctx,r,"kind",kind)) ref.Kind=normalized(kind)=="control"?RuntimeReferenceKind::Control:RuntimeReferenceKind::Binding; JSValue rv; if(property(ctx,r,"source",rv)){ ref.Id=ref.Kind==RuntimeReferenceKind::Control?controlId(ctx,engine,rv):bindingId(ctx,engine,rv); JS_FreeValue(ctx,rv);} intProperty(ctx,r,"signal",ref.Signal); floatProperty(ctx,r,"weight",ref.Weight); boolProperty(ctx,r,"enabled",ref.Enabled); boolProperty(ctx,r,"useOwnComparison",ref.UseOwnComparison); enumProperty(ctx,r,"compareCondition",CMP_NAMES,ref.CompareCondition); floatProperty(ctx,r,"compareA",ref.CompareA); floatProperty(ctx,r,"compareB",ref.CompareB); floatProperty(ctx,r,"compareTolerance",ref.CompareTolerance); if(ref.Id) b.References.push_back(ref); JS_FreeValue(ctx,r);} JS_FreeValue(ctx,p);
            }
            if (property(ctx,o,"links",p) && JS_IsObject(p))
            {
                static constexpr std::pair<const char*,RuntimeParameterSlot> Links[]={{"normalize",RuntimeParameterSlot::Normalize},{"inputMin",RuntimeParameterSlot::InputMin},{"inputMax",RuntimeParameterSlot::InputMax},{"invert",RuntimeParameterSlot::Invert},{"scale",RuntimeParameterSlot::Scale},{"offset",RuntimeParameterSlot::Offset},{"clamp",RuntimeParameterSlot::Clamp},{"outputMin",RuntimeParameterSlot::OutputMin},{"outputMax",RuntimeParameterSlot::OutputMax},{"smoothingHz",RuntimeParameterSlot::SmoothingHz},{"updateHz",RuntimeParameterSlot::UpdateHz}}; for(const auto& [name,slot]:Links){JSValue v;if(property(ctx,p,name,v)){engine.setParameterLink(b,slot,bindingId(ctx,engine,v));JS_FreeValue(ctx,v);}} JS_FreeValue(ctx,p);
            }
        }

        void applyControlConfig(JSContext* ctx, RuntimeBindingEngine& engine, RuntimeControlRule& c, JSValueConst o)
        {
            boolProperty(ctx,o,"enabled",c.Enabled); intProperty(ctx,o,"priority",c.Priority); intProperty(ctx,o,"order",c.Order); std::string s; if(stringProperty(ctx,o,"group",s))copy(c.Group,s); enumProperty(ctx,o,"condition",CTRL_COND_NAMES,c.Condition); floatProperty(ctx,o,"valueA",c.ValueA); floatProperty(ctx,o,"valueB",c.ValueB); floatProperty(ctx,o,"tolerance",c.Tolerance); floatProperty(ctx,o,"hysteresis",c.Hysteresis); enumProperty(ctx,o,"target",CTRL_TARGET_NAMES,c.Target); intProperty(ctx,o,"shaderPresetIndex",c.ShaderPresetIndex); if(stringProperty(ctx,o,"shaderId",s))copy(c.ShaderId,s); floatProperty(ctx,o,"targetValue",c.TargetValue); boolProperty(ctx,o,"targetBool",c.TargetBool); intProperty(ctx,o,"targetComponent",c.TargetComponent); if(stringProperty(ctx,o,"targetId",s))copy(c.TargetId,s); floatProperty(ctx,o,"transitionSeconds",c.TransitionSeconds); boolProperty(ctx,o,"targetUseSourceValue",c.TargetUseSourceValue); if(stringProperty(ctx,o,"stringCompare",s))copy(c.StringCompare,s); boolProperty(ctx,o,"fireOnFirstSample",c.FireOnFirstSample);
            JSValue p; if(property(ctx,o,"source",p)){c.SourceBindingId=bindingId(ctx,engine,p);JS_FreeValue(ctx,p);} if(property(ctx,o,"targetBinding",p)){c.TargetBindingId=bindingId(ctx,engine,p);JS_FreeValue(ctx,p);} if(property(ctx,o,"targetBank",p)){c.TargetBankValueId=bankId(ctx,engine,p);JS_FreeValue(ctx,p);} if(property(ctx,o,"targetControl",p)){c.TargetControlId=controlId(ctx,engine,p);JS_FreeValue(ctx,p);} if(property(ctx,o,"actions",p)){applyActions(ctx,engine,p,c.Actions);JS_FreeValue(ctx,p);}
        }

        JSValue ensureBinding(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx); if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensureBinding(name, config?)"); std::string name;if(!toString(ctx,argv[0],name)||name.empty())return JS_ThrowTypeError(ctx,"binding name required"); auto& engine=*state(ctx)->Engine; RuntimeBinding* b=engine.findBindingByName(name); if(!b){b=&engine.add();copy(b->Name,name);} if(argc>1&&JS_IsObject(argv[1]))applyBindingConfig(ctx,engine,*b,argv[1]); engine.markChanged(); return JS_NewBigUint64(ctx,b->Id);
        }
        JSValue ensureControl(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx); if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensureControl(name, config?)"); std::string name;if(!toString(ctx,argv[0],name)||name.empty())return JS_ThrowTypeError(ctx,"control name required"); auto& engine=*state(ctx)->Engine; RuntimeControlRule* c=engine.findControlByName(name); if(!c){c=&engine.addControl();copy(c->Name,name);} if(argc>1&&JS_IsObject(argv[1]))applyControlConfig(ctx,engine,*c,argv[1]); engine.markChanged(); return JS_NewBigUint64(ctx,c->Id);
        }
        JSValue ensureBank(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx); if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensureBank(name, config?)"); std::string name;if(!toString(ctx,argv[0],name)||name.empty())return JS_ThrowTypeError(ctx,"bank name required"); auto& engine=*state(ctx)->Engine; RuntimeValueBankEntry* v=engine.findBankValueByName(name); if(!v){v=&engine.addBankValue();copy(v->Name,name);} if(argc>1&&JS_IsObject(argv[1])){std::string s;boolProperty(ctx,argv[1],"enabled",v->Enabled);if(stringProperty(ctx,argv[1],"description",s))copy(v->Description,s);enumProperty(ctx,argv[1],"type",{{"Number",RuntimeBankValueType::Number},{"Integer",RuntimeBankValueType::Integer},{"Boolean",RuntimeBankValueType::Boolean},{"String",RuntimeBankValueType::String},{"Address",RuntimeBankValueType::Address}},v->Type);floatProperty(ctx,argv[1],"number",v->Number);boolProperty(ctx,argv[1],"boolean",v->Boolean);if(stringProperty(ctx,argv[1],"string",s))copy(v->String,s);} engine.markChanged(); return JS_NewBigUint64(ctx,v->Id);
        }
        JSValue setBank(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx); if(argc<2)return JS_ThrowTypeError(ctx,"q.graph.setBank(idOrName, value)");auto& engine=*state(ctx)->Engine;auto* v=bankRef(ctx,engine,argv[0]);if(!v)return JS_FALSE;switch(v->Type){case RuntimeBankValueType::Number:{double n=0;if(JS_ToFloat64(ctx,&n,argv[1])<0)return JS_EXCEPTION;v->Number=static_cast<float>(n);break;}case RuntimeBankValueType::Integer:{std::int64_t n=0;if(JS_ToInt64Ext(ctx,&n,argv[1])<0)return JS_EXCEPTION;v->Integer=n;break;}case RuntimeBankValueType::Boolean:{int b=JS_ToBool(ctx,argv[1]);if(b<0)return JS_EXCEPTION;v->Boolean=b!=0;break;}case RuntimeBankValueType::String:{std::string s;if(!toString(ctx,argv[1],s))return JS_EXCEPTION;copy(v->String,s);break;}case RuntimeBankValueType::Address:{std::uint64_t a=0;if(!u64Value(ctx,argv[1],a))return JS_ThrowTypeError(ctx,"address must be integer/BigInt");v->Address=static_cast<std::uintptr_t>(a);break;}}v->HasValue=true;v->ChangedThisFrame=true;engine.markChanged();return JS_TRUE;
        }
        JSValue ensureObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensureObject(name, config?)");std::string name;if(!toString(ctx,argv[0],name))return JS_EXCEPTION;auto& engine=*state(ctx)->Engine;RuntimeObjectDescriptor* o=nullptr;for(auto& x:engine.objects())if(std::string_view(x.Name)==name){o=&x;break;}if(!o){o=&engine.addObject();copy(o->Name,name);}if(argc>1&&JS_IsObject(argv[1])){std::string s;boolProperty(ctx,argv[1],"enabled",o->Enabled);intProperty(ctx,argv[1],"order",o->Order);if(stringProperty(ctx,argv[1],"group",s))copy(o->Group,s);if(stringProperty(ctx,argv[1],"description",s))copy(o->Description,s);enumProperty(ctx,argv[1],"packing",{{"Natural",RuntimeObjectPacking::Natural},{"Pack1",RuntimeObjectPacking::Pack1},{"Pack2",RuntimeObjectPacking::Pack2},{"Pack4",RuntimeObjectPacking::Pack4},{"Pack8",RuntimeObjectPacking::Pack8},{"Pack16",RuntimeObjectPacking::Pack16}},o->Packing);JSValue fields;if(property(ctx,argv[1],"fields",fields)){const auto n=arrayLength(ctx,fields);for(std::uint32_t i=0;i<n;++i){JSValue f=JS_GetPropertyUint32(ctx,fields,i);if(!JS_IsObject(f)){JS_FreeValue(ctx,f);continue;}std::string fn;if(!stringProperty(ctx,f,"name",fn)){JS_FreeValue(ctx,f);continue;}RuntimeObjectField* field=nullptr;for(auto& existing:o->Fields)if(std::string_view(existing.Name)==fn){field=&existing;break;}if(!field){field=&engine.addObjectField(*o);copy(field->Name,fn);}boolProperty(ctx,f,"enabled",field->Enabled);enumProperty(ctx,f,"type",{{"U8",RuntimeObjectFieldType::U8},{"I8",RuntimeObjectFieldType::I8},{"U16",RuntimeObjectFieldType::U16},{"I16",RuntimeObjectFieldType::I16},{"U32",RuntimeObjectFieldType::U32},{"I32",RuntimeObjectFieldType::I32},{"U64",RuntimeObjectFieldType::U64},{"I64",RuntimeObjectFieldType::I64},{"Float",RuntimeObjectFieldType::Float},{"Double",RuntimeObjectFieldType::Double},{"Bool",RuntimeObjectFieldType::Bool},{"Pointer",RuntimeObjectFieldType::Pointer},{"Filler1",RuntimeObjectFieldType::Filler1},{"Filler2",RuntimeObjectFieldType::Filler2},{"Filler4",RuntimeObjectFieldType::Filler4},{"Filler8",RuntimeObjectFieldType::Filler8},{"Filler16",RuntimeObjectFieldType::Filler16},{"Filler32",RuntimeObjectFieldType::Filler32},{"FillerCustom",RuntimeObjectFieldType::FillerCustom},{"CStringPointer",RuntimeObjectFieldType::CStringPointer},{"WStringPointer",RuntimeObjectFieldType::WStringPointer},{"FixedCString",RuntimeObjectFieldType::FixedCString},{"FixedWString",RuntimeObjectFieldType::FixedWString}},field->Type);enumProperty(ctx,f,"alignment",{{"Auto",RuntimeObjectAlignment::Auto},{"Align1",RuntimeObjectAlignment::Align1},{"Align2",RuntimeObjectAlignment::Align2},{"Align4",RuntimeObjectAlignment::Align4},{"Align8",RuntimeObjectAlignment::Align8},{"Align16",RuntimeObjectAlignment::Align16}},field->Alignment);boolProperty(ctx,f,"manualOffset",field->ManualOffset);intProperty(ctx,f,"offset",field->Offset);intProperty(ctx,f,"customFillerBytes",field->CustomFillerBytes);intProperty(ctx,f,"stringMaxLength",field->StringMaxLength);intProperty(ctx,f,"fixedElementCount",field->FixedElementCount);JS_FreeValue(ctx,f);}JS_FreeValue(ctx,fields);}}engine.markChanged();return JS_NewBigUint64(ctx,o->Id);
        }
        JSValue ensurePointer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensurePointer(name, config?)");std::string name;if(!toString(ctx,argv[0],name))return JS_EXCEPTION;auto& engine=*state(ctx)->Engine;RuntimeObjectPointer* p=nullptr;for(auto& x:engine.pointers())if(std::string_view(x.Name)==name){p=&x;break;}if(!p){p=&engine.addPointer();copy(p->Name,name);}if(argc>1&&JS_IsObject(argv[1])){std::string s;boolProperty(ctx,argv[1],"enabled",p->Enabled);intProperty(ctx,argv[1],"order",p->Order);intProperty(ctx,argv[1],"baseOffset",p->BaseOffset);if(stringProperty(ctx,argv[1],"group",s))copy(p->Group,s);JSValue v;if(property(ctx,argv[1],"descriptor",v)){if(auto* o=objectRef(ctx,engine,v))p->DescriptorId=o->Id;JS_FreeValue(ctx,v);}if(property(ctx,argv[1],"baseBinding",v)){p->BaseBindingId=bindingId(ctx,engine,v);JS_FreeValue(ctx,v);}if(property(ctx,argv[1],"processBinding",v)){p->ProcessBindingId=bindingId(ctx,engine,v);JS_FreeValue(ctx,v);}}engine.markChanged();return JS_NewBigUint64(ctx,p->Id);
        }
        void fillIds(JSContext* ctx, JSValueConst array, auto resolver, std::vector<std::uint64_t>& ids){ids.clear();const auto n=arrayLength(ctx,array);for(std::uint32_t i=0;i<n;++i){JSValue v=JS_GetPropertyUint32(ctx,array,i);const auto id=resolver(v);JS_FreeValue(ctx,v);if(id)ids.push_back(id);}}
        JSValue ensureProfile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
        {
            if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<1)return JS_ThrowTypeError(ctx,"q.graph.ensureProfile(name, config?)");std::string name;if(!toString(ctx,argv[0],name))return JS_EXCEPTION;auto& engine=*state(ctx)->Engine;RuntimeBindingProfile* p=nullptr;for(auto& x:engine.profiles())if(std::string_view(x.Name)==name){p=&x;break;}if(!p){p=&engine.addProfile();copy(p->Name,name);}if(argc>1&&JS_IsObject(argv[1])){boolProperty(ctx,argv[1],"enabled",p->Enabled);boolProperty(ctx,argv[1],"exclusive",p->Exclusive);boolProperty(ctx,argv[1],"ctrl",p->HotkeyCtrl);boolProperty(ctx,argv[1],"alt",p->HotkeyAlt);boolProperty(ctx,argv[1],"shift",p->HotkeyShift);intProperty(ctx,argv[1],"key",p->HotkeyKey);JSValue v;if(property(ctx,argv[1],"bindings",v)){fillIds(ctx,v,[&](JSValueConst x){return bindingId(ctx,engine,x);},p->BindingIds);JS_FreeValue(ctx,v);}if(property(ctx,argv[1],"controls",v)){fillIds(ctx,v,[&](JSValueConst x){return controlId(ctx,engine,x);},p->ControlIds);JS_FreeValue(ctx,v);}if(property(ctx,argv[1],"scripts",v)){fillIds(ctx,v,[&](JSValueConst x){if(auto* s=scriptRef(ctx,engine,x))return s->Id;return std::uint64_t{0};},p->ScriptIds);JS_FreeValue(ctx,v);}}engine.markChanged();return JS_NewBigUint64(ctx,p->Id);
        }
        JSValue applyProfile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv){if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<1)return JS_FALSE;auto& e=*state(ctx)->Engine;if(auto* p=profileRef(ctx,e,argv[0])){e.applyProfile(*p);return JS_TRUE;}return JS_FALSE;}
        JSValue activeProfile(JSContext* ctx, JSValueConst, int, JSValueConst*){auto* s=state(ctx);if(!s||!s->Engine)return JS_UNDEFINED;if(auto* p=s->Engine->findProfile(s->Engine->activeProfileId()))return JS_NewString(ctx,p->Name);return JS_UNDEFINED;}
        JSValue setEnabled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic){if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<2)return JS_FALSE;const int enabled=JS_ToBool(ctx,argv[1]);if(enabled<0)return JS_EXCEPTION;auto& e=*state(ctx)->Engine;bool ok=false;if(magic==0){if(auto* x=bindingRef(ctx,e,argv[0])){x->Enabled=enabled;ok=true;}}else if(magic==1){if(auto*x=controlRef(ctx,e,argv[0])){x->Enabled=enabled;ok=true;}}else{if(auto*x=scriptRef(ctx,e,argv[0])){x->Enabled=enabled;ok=true;}}if(ok)e.markChanged();return JS_NewBool(ctx,ok);}
        JSValue operation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv){if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<2)return JS_FALSE;auto& e=*state(ctx)->Engine;auto*b=bindingRef(ctx,e,argv[0]);if(!b)return JS_FALSE;RuntimeBindingOperation op{};if(!enumValue(ctx,argv[1],{{"Refresh",RuntimeBindingOperation::Refresh},{"ForceUpdate",RuntimeBindingOperation::ForceUpdate},{"Invalidate",RuntimeBindingOperation::Invalidate},{"ResetState",RuntimeBindingOperation::ResetState},{"RetryRegisterCapture",RuntimeBindingOperation::RetryRegisterCapture},{"RescanPattern",RuntimeBindingOperation::RescanPattern},{"RebindProcess",RuntimeBindingOperation::RebindProcess},{"ClearError",RuntimeBindingOperation::ClearError}},op))return JS_ThrowTypeError(ctx,"unknown binding operation");e.operateBinding(*b,op);return JS_TRUE;}
        JSValue removeNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic){if(!mutationAllowed(ctx))return mutationError(ctx);if(argc<1)return JS_FALSE;auto&e=*state(ctx)->Engine;if(magic==0){if(auto*x=bindingRef(ctx,e,argv[0])){e.erase(static_cast<std::size_t>(x-e.bindings().data()));return JS_TRUE;}}else if(magic==1){if(auto*x=controlRef(ctx,e,argv[0])){e.eraseControl(static_cast<std::size_t>(x-e.controls().data()));return JS_TRUE;}}else if(magic==2){if(auto*x=bankRef(ctx,e,argv[0])){e.eraseBankValue(static_cast<std::size_t>(x-e.bank().data()));return JS_TRUE;}}else if(magic==3){if(auto*x=profileRef(ctx,e,argv[0])){e.eraseProfile(static_cast<std::size_t>(x-e.profiles().data()));return JS_TRUE;}}else if(magic==4){if(auto*x=pointerRef(ctx,e,argv[0])){e.erasePointer(static_cast<std::size_t>(x-e.pointers().data()));return JS_TRUE;}}else if(magic==5){if(auto*x=objectRef(ctx,e,argv[0])){e.eraseObject(static_cast<std::size_t>(x-e.objects().data()));return JS_TRUE;}}return JS_FALSE;}
        JSValue graphSave(JSContext* ctx,JSValueConst,int,JSValueConst*){if(!mutationAllowed(ctx))return mutationError(ctx);return JS_NewBool(ctx,state(ctx)->Engine->save());}

        JSValue runtimeShader(JSContext* ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;std::string id;if(!toString(ctx,argv[0],id))return JS_EXCEPTION;s->Output->ShaderId=id;if(argc>1){double d=0;if(JS_ToFloat64(ctx,&d,argv[1])>=0)s->Output->ShaderTransitionSeconds=static_cast<float>(std::max(d,0.0));}return JS_TRUE;}
        JSValue runtimeShaderPreset(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;std::int32_t index=0;if(JS_ToInt32(ctx,&index,argv[0])<0)return JS_EXCEPTION;s->Output->ShaderPresetIndex=index;if(argc>1){double d=0;if(JS_ToFloat64(ctx,&d,argv[1])>=0)s->Output->ShaderTransitionSeconds=static_cast<float>(std::max(d,0.0));}return JS_TRUE;}
        JSValue runtimeBrightness(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;double v=0;if(JS_ToFloat64(ctx,&v,argv[0])<0)return JS_EXCEPTION;s->Output->GlobalBrightness=static_cast<float>(v);return JS_TRUE;}
        JSValue runtimeSend(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;const int v=JS_ToBool(ctx,argv[0]);if(v<0)return JS_EXCEPTION;s->Output->SendFramebuffer=v!=0;return JS_TRUE;}
        JSValue runtimeBaseMode(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;std::int32_t v=0;if(JS_ToInt32(ctx,&v,argv[0])<0)return JS_EXCEPTION;s->Output->BaseColorMode=v;return JS_TRUE;}
        JSValue runtimeMaterial(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Shader||argc<3)return JS_FALSE;std::string id;if(!toString(ctx,argv[0],id))return JS_EXCEPTION;std::int32_t component=0;double value=0;if(JS_ToInt32(ctx,&component,argv[1])<0||JS_ToFloat64(ctx,&value,argv[2])<0)return JS_EXCEPTION;return JS_NewBool(ctx,s->Shader->setMaterialParameter(id,component,static_cast<float>(value)));}
        JSValue runtimeCurrentShader(JSContext*ctx,JSValueConst,int,JSValueConst*){auto*s=state(ctx);return s&&s->SignalContext?JS_NewString(ctx,s->SignalContext->CurrentShaderId.c_str()):JS_UNDEFINED;}
        JSValue runtimePreviousShader(JSContext*ctx,JSValueConst,int,JSValueConst*){auto*s=state(ctx);return s&&s->SignalContext?JS_NewString(ctx,s->SignalContext->PreviousShaderId.c_str()):JS_UNDEFINED;}
    }

    void runtimeInstallQuickJSGraphApi(JSContext* ctx, JSValueConst api)
    {
        JSValue graph=JS_NewObject(ctx);JS_SetPropertyStr(ctx,graph,"ensureBinding",JS_NewCFunction(ctx,ensureBinding,"ensureBinding",2));JS_SetPropertyStr(ctx,graph,"ensureControl",JS_NewCFunction(ctx,ensureControl,"ensureControl",2));JS_SetPropertyStr(ctx,graph,"ensureBank",JS_NewCFunction(ctx,ensureBank,"ensureBank",2));JS_SetPropertyStr(ctx,graph,"setBank",JS_NewCFunction(ctx,setBank,"setBank",2));JS_SetPropertyStr(ctx,graph,"ensureObject",JS_NewCFunction(ctx,ensureObject,"ensureObject",2));JS_SetPropertyStr(ctx,graph,"ensurePointer",JS_NewCFunction(ctx,ensurePointer,"ensurePointer",2));JS_SetPropertyStr(ctx,graph,"ensureProfile",JS_NewCFunction(ctx,ensureProfile,"ensureProfile",2));JS_SetPropertyStr(ctx,graph,"applyProfile",JS_NewCFunction(ctx,applyProfile,"applyProfile",1));JS_SetPropertyStr(ctx,graph,"activeProfile",JS_NewCFunction(ctx,activeProfile,"activeProfile",0));JS_SetPropertyStr(ctx,graph,"bindingOperation",JS_NewCFunction(ctx,operation,"bindingOperation",2));JS_SetPropertyStr(ctx,graph,"setBindingEnabled",JS_NewCFunctionMagic(ctx,setEnabled,"setBindingEnabled",2,JS_CFUNC_generic_magic,0));JS_SetPropertyStr(ctx,graph,"setControlEnabled",JS_NewCFunctionMagic(ctx,setEnabled,"setControlEnabled",2,JS_CFUNC_generic_magic,1));JS_SetPropertyStr(ctx,graph,"setScriptEnabled",JS_NewCFunctionMagic(ctx,setEnabled,"setScriptEnabled",2,JS_CFUNC_generic_magic,2));JS_SetPropertyStr(ctx,graph,"removeBinding",JS_NewCFunctionMagic(ctx,removeNode,"removeBinding",1,JS_CFUNC_generic_magic,0));JS_SetPropertyStr(ctx,graph,"removeControl",JS_NewCFunctionMagic(ctx,removeNode,"removeControl",1,JS_CFUNC_generic_magic,1));JS_SetPropertyStr(ctx,graph,"removeBank",JS_NewCFunctionMagic(ctx,removeNode,"removeBank",1,JS_CFUNC_generic_magic,2));JS_SetPropertyStr(ctx,graph,"removeProfile",JS_NewCFunctionMagic(ctx,removeNode,"removeProfile",1,JS_CFUNC_generic_magic,3));JS_SetPropertyStr(ctx,graph,"removePointer",JS_NewCFunctionMagic(ctx,removeNode,"removePointer",1,JS_CFUNC_generic_magic,4));JS_SetPropertyStr(ctx,graph,"removeObject",JS_NewCFunctionMagic(ctx,removeNode,"removeObject",1,JS_CFUNC_generic_magic,5));JS_SetPropertyStr(ctx,graph,"save",JS_NewCFunction(ctx,graphSave,"save",0));JS_SetPropertyStr(ctx,api,"graph",graph);
        JSValue runtime=JS_NewObject(ctx);JS_SetPropertyStr(ctx,runtime,"shader",JS_NewCFunction(ctx,runtimeShader,"shader",2));JS_SetPropertyStr(ctx,runtime,"shaderPreset",JS_NewCFunction(ctx,runtimeShaderPreset,"shaderPreset",2));JS_SetPropertyStr(ctx,runtime,"brightness",JS_NewCFunction(ctx,runtimeBrightness,"brightness",1));JS_SetPropertyStr(ctx,runtime,"sendFramebuffer",JS_NewCFunction(ctx,runtimeSend,"sendFramebuffer",1));JS_SetPropertyStr(ctx,runtime,"baseColorMode",JS_NewCFunction(ctx,runtimeBaseMode,"baseColorMode",1));JS_SetPropertyStr(ctx,runtime,"material",JS_NewCFunction(ctx,runtimeMaterial,"material",3));JS_SetPropertyStr(ctx,runtime,"currentShader",JS_NewCFunction(ctx,runtimeCurrentShader,"currentShader",0));JS_SetPropertyStr(ctx,runtime,"previousShader",JS_NewCFunction(ctx,runtimePreviousShader,"previousShader",0));JS_SetPropertyStr(ctx,api,"runtime",runtime);
    }
}
''')

# --- Global workspace runtime, external watcher and q.import ---
write("src/runtime/QuickJSWorkspace.cpp", r'''#include "QuickJSInternal.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include <quickjs.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t WorkspaceMemoryLimit=64ULL*1024ULL*1024ULL, WorkspaceStackLimit=512ULL*1024ULL;
        constexpr double MaximumSafeInteger=9007199254740991.0;
        std::uint64_t hashText(const std::string_view text) noexcept { std::uint64_t h=14695981039346656037ULL;for(const unsigned char c:text){h^=c;h*=1099511628211ULL;}return h; }
        std::string exceptionText(JSContext* ctx){JSValue ex=JS_GetException(ctx);std::string out="QuickJS exception";if(const char*t=JS_ToCString(ctx,ex)){out=t;JS_FreeCString(ctx,t);}if(JS_IsObject(ex)){JSValue stack=JS_GetPropertyStr(ctx,ex,"stack");if(const char*t=JS_ToCString(ctx,stack)){if(*t){out+='\n';out+=t;}JS_FreeCString(ctx,t);}JS_FreeValue(ctx,stack);}JS_FreeValue(ctx,ex);return out;}
        struct Instance:RuntimeQuickJSContext
        {
            JSContext* Context=nullptr;JSValue Function=JS_UNDEFINED,Api=JS_UNDEFINED;std::uint64_t SourceHash=0;std::filesystem::path MainPath,CurrentDirectory;std::unordered_map<std::string,std::filesystem::file_time_type> DependencyTimes;std::unordered_map<std::string,JSValue> Modules;
            ~Instance(){if(!Context)return;for(auto&[_,v]:Modules)JS_FreeValue(Context,v);JS_FreeValue(Context,Function);JS_FreeValue(Context,Api);JS_FreeContext(Context);}
        };
        struct Workspace
        {
            JSRuntime* Runtime=nullptr;RuntimeQuickJSDeadline Execution{};std::unordered_map<std::uint64_t,std::unique_ptr<Instance>> Instances;
            Workspace(){Runtime=JS_NewRuntime();if(Runtime){JS_SetMemoryLimit(Runtime,WorkspaceMemoryLimit);JS_SetMaxStackSize(Runtime,WorkspaceStackLimit);JS_SetRuntimeOpaque(Runtime,this);JS_SetInterruptHandler(Runtime,[](JSRuntime*,void* opaque){auto*self=static_cast<Workspace*>(opaque);if(!self||!self->Execution.Active||std::chrono::steady_clock::now()<self->Execution.Deadline)return 0;self->Execution.Interrupted=true;return 1;},this);}}
            ~Workspace(){Instances.clear();if(Runtime)JS_FreeRuntime(Runtime);}
            void begin(float ms){Execution.Interrupted=false;Execution.Active=true;Execution.Deadline=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double,std::milli>(std::clamp(ms,0.1f,100.0f)));}void end(){Execution.Active=false;}
        };
        Workspace& workspace(){static Workspace w;return w;}
        RuntimeBinding* resolveBinding(JSContext*ctx,JSValueConst v){auto*s=static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));if(!s||!s->Engine)return nullptr;if(JS_IsNumber(v)||JS_IsBigInt(ctx,v)){std::int64_t id=0;if(JS_ToInt64Ext(ctx,&id,v)<0||id<=0)return nullptr;return s->Engine->findBinding(static_cast<std::uint64_t>(id));}const char*n=JS_ToCString(ctx,v);if(!n)return nullptr;auto*r=s->Engine->findBindingByName(n);JS_FreeCString(ctx,n);return r;}
        RuntimeControlRule* resolveControl(JSContext*ctx,JSValueConst v){auto*s=static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));if(!s||!s->Engine)return nullptr;if(JS_IsNumber(v)||JS_IsBigInt(ctx,v)){std::int64_t id=0;if(JS_ToInt64Ext(ctx,&id,v)<0||id<=0)return nullptr;return s->Engine->findControl(static_cast<std::uint64_t>(id));}const char*n=JS_ToCString(ctx,v);if(!n)return nullptr;auto*r=s->Engine->findControlByName(n);JS_FreeCString(ctx,n);return r;}
        RuntimeValueBankEntry* resolveBank(JSContext*ctx,JSValueConst v){auto*s=static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));if(!s||!s->Engine)return nullptr;if(JS_IsNumber(v)||JS_IsBigInt(ctx,v)){std::int64_t id=0;if(JS_ToInt64Ext(ctx,&id,v)<0||id<=0)return nullptr;return s->Engine->findBankValue(static_cast<std::uint64_t>(id));}const char*n=JS_ToCString(ctx,v);if(!n)return nullptr;auto*r=s->Engine->findBankValueByName(n);JS_FreeCString(ctx,n);return r;}
        JSValue jsBinding(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*b=argc?resolveBinding(ctx,argv[0]):nullptr;return b&&b->Enabled&&b->HasValue?JS_NewFloat64(ctx,b->Value):JS_UNDEFINED;}
        JSValue jsRaw(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*b=argc?resolveBinding(ctx,argv[0]):nullptr;return b&&b->Enabled&&b->HasValue?JS_NewFloat64(ctx,b->RawValue):JS_UNDEFINED;}
        JSValue jsText(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*b=argc?resolveBinding(ctx,argv[0]):nullptr;return b&&b->Enabled&&b->HasString?JS_NewString(ctx,b->StringValue.c_str()):JS_UNDEFINED;}
        JSValue jsAddress(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*b=argc?resolveBinding(ctx,argv[0]):nullptr;return b&&b->Enabled&&b->HasAddress?JS_NewBigUint64(ctx,b->AddressValue):JS_UNDEFINED;}
        JSValue jsBank(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*v=argc?resolveBank(ctx,argv[0]):nullptr;if(!v||!v->Enabled||!v->HasValue)return JS_UNDEFINED;switch(v->Type){case RuntimeBankValueType::Number:return JS_NewFloat64(ctx,v->Number);case RuntimeBankValueType::Integer:return std::abs(static_cast<double>(v->Integer))<=MaximumSafeInteger?JS_NewFloat64(ctx,static_cast<double>(v->Integer)):JS_NewBigInt64(ctx,v->Integer);case RuntimeBankValueType::Boolean:return JS_NewBool(ctx,v->Boolean);case RuntimeBankValueType::String:return JS_NewString(ctx,v->String);case RuntimeBankValueType::Address:return v->Address?JS_NewBigUint64(ctx,v->Address):JS_UNDEFINED;}return JS_UNDEFINED;}
        JSValue jsControl(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*c=argc?resolveControl(ctx,argv[0]):nullptr;return c&&c->Enabled&&c->RuntimeEnabled?JS_NewBool(ctx,c->ConditionActive):JS_UNDEFINED;}
        JSValue jsTriggered(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*c=argc?resolveControl(ctx,argv[0]):nullptr;return c&&c->Enabled&&c->RuntimeEnabled?JS_NewBool(ctx,c->TriggeredThisFrame):JS_UNDEFINED;}
        JSValue jsLog(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));if(!s||!s->Script)return JS_UNDEFINED;std::string text;for(int i=0;i<argc;++i){const char*v=JS_ToCString(ctx,argv[i]);if(!v)return JS_EXCEPTION;if(!text.empty())text.push_back(' ');if(text.size()<2048)text.append(v,std::min<std::size_t>(std::strlen(v),2048-text.size()));JS_FreeCString(ctx,v);}s->Script->LastLog=std::move(text);return JS_UNDEFINED;}
        bool loadFile(const std::filesystem::path& path,std::string& text){std::ifstream file(path,std::ios::binary);if(!file)return false;text.assign(std::istreambuf_iterator<char>(file),{});return static_cast<bool>(file)||file.eof();}
        std::filesystem::file_time_type fileTime(const std::filesystem::path& path){std::error_code ec;const auto t=std::filesystem::last_write_time(path,ec);return ec?std::filesystem::file_time_type::min():t;}
        JSValue jsImport(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv)
        {
            if(argc<1)return JS_ThrowTypeError(ctx,"q.import(path): path required");auto*base=static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx));auto*instance=static_cast<Instance*>(base);const char*raw=JS_ToCString(ctx,argv[0]);if(!raw)return JS_EXCEPTION;std::filesystem::path requested(raw);JS_FreeCString(ctx,raw);std::filesystem::path path=requested.is_absolute()?requested:instance->CurrentDirectory/requested;std::error_code ec;path=std::filesystem::weakly_canonical(path,ec);if(ec)path=std::filesystem::absolute(path,ec);if(path.extension()!=".js"&&path.extension()!=".mjs")return JS_ThrowTypeError(ctx,"q.import only accepts .js/.mjs files");const std::string key=path.string();if(const auto it=instance->Modules.find(key);it!=instance->Modules.end())return JS_DupValue(ctx,it->second);std::string source;if(!loadFile(path,source))return JS_ThrowReferenceError(ctx,"could not import %s",key.c_str());instance->DependencyTimes[key]=fileTime(path);std::string wrapped="(function(q,exports,module){\n\"use strict\";\n"+source+"\n;return module.exports;\n})";auto&w=workspace();w.begin(instance->Script?instance->Script->TimeoutMs:4.0f);JSValue fn=JS_Eval(ctx,wrapped.c_str(),wrapped.size(),key.c_str(),JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_STRICT);w.end();if(JS_IsException(fn))return fn;JSValue exports=JS_NewObject(ctx),module=JS_NewObject(ctx);JS_SetPropertyStr(ctx,module,"exports",JS_DupValue(ctx,exports));JSValue args[3]{JS_DupValue(ctx,instance->Api),JS_DupValue(ctx,exports),JS_DupValue(ctx,module)};const auto old=instance->CurrentDirectory;instance->CurrentDirectory=path.parent_path();w.begin(instance->Script?instance->Script->TimeoutMs:4.0f);JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,3,args);w.end();instance->CurrentDirectory=old;for(auto&a:args)JS_FreeValue(ctx,a);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,exports);JS_FreeValue(ctx,module);if(JS_IsException(result))return result;instance->Modules.emplace(key,JS_DupValue(ctx,result));return result;
        }
        Instance* createInstance(RuntimeBindingEngine& engine,RuntimeScript& script,const RuntimeSignalContext& signal,ShaderFramebuffer& shader,RuntimeControlOutput& output)
        {
            auto&w=workspace();if(!w.Runtime)return nullptr;auto[it,inserted]=w.Instances.try_emplace(script.Id);if(!inserted)return it->second.get();auto instance=std::make_unique<Instance>();instance->Execution=&w.Execution;instance->Engine=&engine;instance->Script=&script;instance->SignalContext=&signal;instance->Shader=&shader;instance->Output=&output;instance->AllowGraphMutation=true;instance->Context=JS_NewContext(w.Runtime);if(!instance->Context){w.Instances.erase(it);return nullptr;}JS_SetContextOpaque(instance->Context,static_cast<RuntimeQuickJSContext*>(instance.get()));instance->Api=JS_NewObject(instance->Context);JS_SetPropertyStr(instance->Context,instance->Api,"binding",JS_NewCFunction(instance->Context,jsBinding,"binding",1));JS_SetPropertyStr(instance->Context,instance->Api,"raw",JS_NewCFunction(instance->Context,jsRaw,"raw",1));JS_SetPropertyStr(instance->Context,instance->Api,"text",JS_NewCFunction(instance->Context,jsText,"text",1));JS_SetPropertyStr(instance->Context,instance->Api,"address",JS_NewCFunction(instance->Context,jsAddress,"address",1));JS_SetPropertyStr(instance->Context,instance->Api,"bank",JS_NewCFunction(instance->Context,jsBank,"bank",1));JS_SetPropertyStr(instance->Context,instance->Api,"control",JS_NewCFunction(instance->Context,jsControl,"control",1));JS_SetPropertyStr(instance->Context,instance->Api,"triggered",JS_NewCFunction(instance->Context,jsTriggered,"triggered",1));JS_SetPropertyStr(instance->Context,instance->Api,"log",JS_NewCFunction(instance->Context,jsLog,"log",1));JS_SetPropertyStr(instance->Context,instance->Api,"import",JS_NewCFunction(instance->Context,jsImport,"import",1));runtimeInstallQuickJSLowLevelApi(instance->Context,instance->Api);runtimeInstallQuickJSGraphApi(instance->Context,instance->Api);JS_DefinePropertyValueStr(instance->Context,instance->Api,"state",JS_NewObject(instance->Context),JS_PROP_ENUMERABLE);auto*result=instance.get();it->second=std::move(instance);return result;
        }
        bool dependenciesChanged(const Instance& instance){for(const auto&[path,time]:instance.DependencyTimes)if(fileTime(path)!=time)return true;return false;}
        bool compile(Instance& instance,RuntimeScript& script,const std::string& source,const std::filesystem::path& filename,std::string& error)
        {
            const auto hash=hashText(source);if(instance.SourceHash==hash&&JS_IsFunction(instance.Context,instance.Function))return true;JS_FreeValue(instance.Context,instance.Function);instance.Function=JS_UNDEFINED;instance.SourceHash=hash;for(auto&[_,v]:instance.Modules)JS_FreeValue(instance.Context,v);instance.Modules.clear();instance.DependencyTimes.clear();instance.MainPath=filename;instance.CurrentDirectory=filename.empty()?runtimeQuickJSScriptDirectory():filename.parent_path();std::string wrapped="(function(q){\n\"use strict\";\n"+source+"\n})";auto&w=workspace();w.begin(script.TimeoutMs);JSValue fn=JS_Eval(instance.Context,wrapped.c_str(),wrapped.size(),filename.empty()?"quartz-workspace.js":filename.string().c_str(),JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_STRICT);w.end();if(JS_IsException(fn)){if(w.Execution.Interrupted){JSValue ex=JS_GetException(instance.Context);JS_FreeValue(instance.Context,ex);++script.TimeoutCount;error="QuickJS compile timed out";}else error=exceptionText(instance.Context);return false;}instance.Function=fn;++script.CompileCount;return true;
        }
        bool evaluate(RuntimeBindingEngine&engine,RuntimeScript&script,const RuntimeSignalContext&signal,ShaderFramebuffer&shader,RuntimeControlOutput&output)
        {
            auto&w=workspace();auto existing=w.Instances.find(script.Id);if(existing!=w.Instances.end()&&engine.scriptSettings().ExternalHotReload&&script.HotReload&&dependenciesChanged(*existing->second)){w.Instances.erase(existing);++script.ReloadCount;}
            std::string source;std::filesystem::path filename;if(script.External){filename=script.Path;if(filename.is_relative())filename=runtimeQuickJSScriptDirectory()/filename;if(!loadFile(filename,source)){script.Status="could not read external script: "+filename.string();return false;}}else source=script.Source;
            Instance*instance=createInstance(engine,script,signal,shader,output);if(!instance){script.Status="could not create QuickJS workspace context";return false;}instance->Engine=&engine;instance->Script=&script;instance->SignalContext=&signal;instance->Shader=&shader;instance->Output=&output;std::string error;if(!compile(*instance,script,source,filename,error)){script.Status=std::move(error);return false;}JS_SetPropertyStr(instance->Context,instance->Api,"time",JS_NewFloat64(instance->Context,signal.Time));JS_SetPropertyStr(instance->Context,instance->Api,"deltaTime",JS_NewFloat64(instance->Context,signal.DeltaTime));JS_SetPropertyStr(instance->Context,instance->Api,"id",JS_NewBigUint64(instance->Context,script.Id));JS_SetPropertyStr(instance->Context,instance->Api,"name",JS_NewString(instance->Context,script.Name));JSValue arg=JS_DupValue(instance->Context,instance->Api);const auto started=std::chrono::steady_clock::now();w.begin(script.TimeoutMs);JSValue result=JS_Call(instance->Context,instance->Function,JS_UNDEFINED,1,&arg);w.end();script.LastMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();++script.RunCount;JS_FreeValue(instance->Context,arg);if(JS_IsException(result)){if(w.Execution.Interrupted){JSValue ex=JS_GetException(instance->Context);JS_FreeValue(instance->Context,ex);++script.TimeoutCount;script.Status="execution timed out after "+std::to_string(script.TimeoutMs)+" ms";}else script.Status=exceptionText(instance->Context);return false;}JS_FreeValue(instance->Context,result);script.Dependencies.clear();for(const auto&[path,_]:instance->DependencyTimes)script.Dependencies.push_back(path);script.Status="running";return true;
        }
    }

    RuntimeControlOutput runtimeEvaluateWorkspaceScripts(RuntimeBindingEngine& engine,const RuntimeSignalContext& context,ShaderFramebuffer& shader)
    {
        RuntimeControlOutput output;for(auto&script:engine.scripts()){if(!script.Enabled||context.Time<script.NextUpdate)continue;const float hz=std::clamp(script.UpdateHz,0.5f,500.0f);script.NextUpdate=context.Time+1.0/hz;evaluate(engine,script,context,shader,output);}return output;
    }
    void runtimeResetWorkspaceScript(const std::uint64_t id) noexcept {workspace().Instances.erase(id);if(workspace().Runtime)JS_RunGC(workspace().Runtime);}
    void runtimeReloadAllWorkspaceScripts() noexcept {auto&w=workspace();for(auto&[_,instance]:w.Instances)if(instance&&instance->Script)++instance->Script->ReloadCount;w.Instances.clear();if(w.Runtime)JS_RunGC(w.Runtime);}
    std::filesystem::path runtimeQuickJSScriptDirectory(){return settingsPath().parent_path()/"scripts";}
}
''')

# --- Built-in Terraria/Astroflux migration preset ---
write("src/runtime/QuickJSPresets.cpp", r'''#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include <fstream>

namespace quartz::client
{
    std::string_view runtimeTerrariaAstrofluxMigrationScript() noexcept
    {
        static constexpr std::string_view Source = R"JS(// Migrated from Quartz runtime material bindings v11.
// Low-level native/object bindings remain graph nodes because they already handle process reattach,
// register-relative signature capture and object layout. Derived state + shader automation lives here.

if (!q.state.configured) {
    q.graph.ensureBinding("Native::Terraria.Player*::LocalInstance", {
        source: "NativeAddress", signal: 1, writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60,
        processName: "Terraria.bin.x86_64", rebindMode: "ExecutableExact",
        rebindPattern: "/home/raony/.local/share/Steam/steamapps/common/Terraria/Terraria.bin.x86_64",
        addressMode: "Signature", signatureExecutableOnly: true,
        signature: "49 8B C7 48 63 80 1C 08 00 00 F3 0F 2A C0 49 63 87 18 08 00 00 F3 0F 2A C8 F3 0F 5E C1 F3 0F 10 0D ?? ?? ?? ?? F3 0F 59 C1",
        signatureResolve: "RegisterRelativeCapture", signatureInstructionSize: 7, signatureRegister: "r15",
        signatureRegisterDisplacementOffset: 3, signatureDisplacementType: "I32", signatureCaptureTimeoutSeconds: 10,
        priority: 0, order: 0, group: "Terraria / Address Resolution"
    });
    q.graph.ensureBinding("Native::Terraria.World::Active", {
        source: "NativeProcess", valueType: "bool", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60,
        processName: "Terraria.bin.x86_64", rebindMode: "ExecutableExact",
        rebindPattern: "/home/raony/.local/share/Steam/steamapps/common/Terraria/Terraria.bin.x86_64",
        addressMode: "Signature", signatureExecutableOnly: true,
        signature: "F7 00 01 00 00 00 74 08 66 66 90 E8 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 0F B6 00 85 C0 0F 84 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 0F B6 00 85 C0 75 ??",
        signatureResolve: "Address32AtOffset", signatureResultOffset: 17, signatureInstructionSize: 5,
        priority: 5, order: 0, group: "Terraria / Address Resolution"
    });
    q.graph.ensureObject("Terraria.Player", {description: "Native model for the local Terraria Player object", fields: [
        {name: "Reserved_0000_0813", type: "FillerCustom", customFillerBytes: 0x814},
        {name: "statLifeMax2", type: "U32"}, {name: "Reserved_0818_081B", type: "Filler4"}, {name: "statLife", type: "U32"}
    ]});
    q.graph.ensurePointer("Pointer::Terraria.Player::Local", {descriptor: "Terraria.Player", baseBinding: "Native::Terraria.Player*::LocalInstance", group: "Terraria / Pointer Instances"});
    q.graph.ensureBinding("Model::Terraria.Player::statLife", {source: "ObjectField", object: "Terraria.Player", pointer: "Pointer::Terraria.Player::Local", field: "statLife", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Terraria / Player Model"});
    q.graph.ensureBinding("Model::Terraria.Player::statLifeMax2", {source: "ObjectField", object: "Terraria.Player", pointer: "Pointer::Terraria.Player::Local", field: "statLifeMax2", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Terraria / Player Model"});

    q.graph.ensureBinding("Native::Astroflux.PlayerShip*::LocalInstance", {source: "NativeAddress", signal: 1, writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, processName: "Astroflux.exe", rebindMode: "NameExact", module: "Astroflux.exe", addressMode: "AddressChain", address: "+0x6137C28", priority: 0, group: "Astroflux / Address Resolution"});
    q.graph.ensureObject("Astroflux.PlayerShip", {description: "Local Astroflux PlayerShip layout", fields: [
        {name: "shieldHp", type: "I32", manualOffset: true, offset: 0xAC},
        {name: "shieldHpMax", type: "I32", manualOffset: true, offset: 0xB0}
    ]});
    q.graph.ensurePointer("Pointer::Astroflux.PlayerShip::Local", {descriptor: "Astroflux.PlayerShip", baseBinding: "Native::Astroflux.PlayerShip*::LocalInstance", group: "Astroflux / Pointer Instances"});
    q.graph.ensureBinding("Model::Astroflux.PlayerShip::shieldHp", {source: "ObjectField", object: "Astroflux.PlayerShip", pointer: "Pointer::Astroflux.PlayerShip::Local", field: "shieldHp", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Astroflux / PlayerShip Model"});
    q.graph.ensureBinding("Model::Astroflux.PlayerShip::shieldHpMax", {source: "ObjectField", object: "Astroflux.PlayerShip", pointer: "Pointer::Astroflux.PlayerShip::Local", field: "shieldHpMax", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Astroflux / PlayerShip Model"});

    q.graph.ensureBank("Shader::SavedPreTerrariaId", {type: "String", description: "Shader active before Terraria took ownership"});
    q.graph.ensureBank("Shader::SavedPreAstrofluxId", {type: "String", description: "Shader active before Astroflux took ownership"});

    // The JS runtime replaces these old control nodes if this migration is installed over a v11 graph.
    ["Automation::Terraria.CapturePreWorldShader", "Automation::Terraria.HealthVisualization", "Automation::Terraria.RestorePreviousShader", "Automation::Terraria.DeathVisualization", "Automation::Terraria.WorldStateCoordinator", "Automation::Terraria.RespawnVisualization", "Automation::Astroflux.ShieldVisualization", "Automation::Astroflux.RestorePreviousShader", "Automation::Astroflux.ShieldDepletedVisualization"].forEach(name => q.graph.setControlEnabled(name, false));

    const terrariaBindings = ["Native::Terraria.Player*::LocalInstance", "Native::Terraria.World::Active", "Model::Terraria.Player::statLife", "Model::Terraria.Player::statLifeMax2"];
    const astroBindings = ["Native::Astroflux.PlayerShip*::LocalInstance", "Model::Astroflux.PlayerShip::shieldHp", "Model::Astroflux.PlayerShip::shieldHpMax"];
    q.graph.ensureProfile("Terraria Runtime - Enabled", {exclusive: true, bindings: terrariaBindings, scripts: [q.name]});
    q.graph.ensureProfile("Terraria Runtime - Disabled", {exclusive: true, bindings: [], scripts: []});
    q.graph.ensureProfile("Astroflux Runtime - Enabled", {exclusive: true, bindings: astroBindings, scripts: [q.name]});
    q.graph.ensureProfile("Astroflux Runtime - Disabled", {exclusive: true, bindings: [], scripts: []});
    q.state.configured = true;
}

const profile = q.graph.activeProfile();
const allowTerraria = !profile || profile === "Terraria Runtime - Enabled";
const allowAstroflux = !profile || profile === "Astroflux Runtime - Enabled";
const worldActive = allowTerraria && !!q.binding("Native::Terraria.World::Active");
const life = q.binding("Model::Terraria.Player::statLife");
const lifeMax = q.binding("Model::Terraria.Player::statLifeMax2");
const terrariaReadable = worldActive && Number.isFinite(life) && Number.isFinite(lifeMax) && lifeMax > 0;

if (worldActive && !q.state.terrariaWorldActive) q.graph.setBank("Shader::SavedPreTerrariaId", q.runtime.currentShader());
if (terrariaReadable) {
    const fraction = Math.max(0, Math.min(1, life / lifeMax));
    q.runtime.material("source.health", 0, fraction);
    q.runtime.shader(life <= 0 ? "shader.hp.death" : "shader.hp.slider", 0.35);
}
if (!worldActive && q.state.terrariaWorldActive) {
    const saved = q.bank("Shader::SavedPreTerrariaId");
    if (saved) q.runtime.shader(saved, 0.35);
}
q.state.terrariaWorldActive = worldActive;

const shield = q.binding("Model::Astroflux.PlayerShip::shieldHp");
const shieldMax = q.binding("Model::Astroflux.PlayerShip::shieldHpMax");
const astroReadable = allowAstroflux && Number.isFinite(shield) && Number.isFinite(shieldMax) && shieldMax > 0;
if (astroReadable && !q.state.astroReadable) q.graph.setBank("Shader::SavedPreAstrofluxId", q.runtime.currentShader());
if (astroReadable) {
    const fraction = Math.max(0, Math.min(1, shield / shieldMax));
    q.runtime.material("source.health", 0, fraction);
    q.runtime.shader(shield <= 0 ? "shader.hp.death" : "shader.hp.slider", 0.35);
}
if (!astroReadable && q.state.astroReadable) {
    const saved = q.bank("Shader::SavedPreAstrofluxId");
    if (saved) q.runtime.shader(saved, 0.35);
}
q.state.astroReadable = astroReadable;
)JS";
        return Source;
    }

    bool runtimeInstallTerrariaAstrofluxMigration(RuntimeBindingEngine& engine, std::string& error)
    {
        const auto directory=runtimeQuickJSScriptDirectory();std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec){error=ec.message();return false;}const auto path=directory/"terraria-astroflux-runtime.js";std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file){error="could not write "+path.string();return false;}const auto source=runtimeTerrariaAstrofluxMigrationScript();file.write(source.data(),static_cast<std::streamsize>(source.size()));if(!file){error="failed writing "+path.string();return false;}RuntimeScript* script=engine.findScriptByName("Terraria + Astroflux runtime");if(!script){script=&engine.addScript();std::snprintf(script->Name,sizeof(script->Name),"Terraria + Astroflux runtime");}script->External=true;script->Path=path.string();script->HotReload=true;script->UpdateHz=60.0f;script->TimeoutMs=8.0f;script->Enabled=true;std::snprintf(script->Group,sizeof(script->Group),"Games");engine.markChanged();runtimeResetWorkspaceScript(script->Id);error.clear();return true;
    }
}
''')

# --- JavaScript page ---
write("include/quartz/client/ui/pages/JavaScriptPage.hpp", r'''#pragma once
#include "quartz/client/ui/Page.hpp"

namespace quartz::client::ui
{
    class JavaScriptPage final : public Page
    {
    public:
        [[nodiscard]] std::string_view id() const noexcept override { return "javascript"; }
        [[nodiscard]] std::string_view title() const noexcept override { return "JavaScript"; }
        [[nodiscard]] PageSection section() const noexcept override { return PageSection::Runtime; }
        void render(PageContext& context, PageManager& manager) override;
    };
}
''')
write("src/ui/pages/JavaScriptPage.cpp", r'''#include "quartz/client/ui/pages/JavaScriptPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <TextEditor.h>
#include <memory>
#include <unordered_map>

namespace quartz::client::ui
{
    namespace
    {
        const TextEditor::Language* javascriptLanguage()
        {
            static const TextEditor::Language language=[] { TextEditor::Language v;v.name="JavaScript";v.singleLineComment="//";v.commentStart="/*";v.commentEnd="*/";v.hasSingleQuotedStrings=true;v.hasDoubleQuotedStrings=true;v.otherStringStart="`";v.otherStringEnd="`";v.stringEscape='\\';v.keywords={"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","from","function","get","if","import","in","instanceof","let","new","of","return","set","static","super","switch","this","throw","try","typeof","var","void","while","with","yield"};v.declarations={"true","false","null","undefined"};v.identifiers={"q","Math","JSON","BigInt","Number","String","Boolean","Array","Object","Map","Set","Date","RegExp","NaN","Infinity"};v.isPunctuation=[](const ImWchar c){return std::string_view("[]{}().,;:+-*/%<>=!&|^~?").find(static_cast<char>(c))!=std::string_view::npos;};return v;}();return &language;
        }
        struct EditorState{TextEditor Editor;std::string Synced;bool Init=false;};
        EditorState& editor(RuntimeScript& script){static std::unordered_map<std::uint64_t,std::unique_ptr<EditorState>> editors;auto[it,inserted]=editors.try_emplace(script.Id);if(inserted)it->second=std::make_unique<EditorState>();auto&s=*it->second;if(!s.Init){s.Editor.SetLanguage(javascriptLanguage());s.Editor.SetPalette(shaderEditorPalette());s.Editor.SetTabSize(4);s.Editor.SetInsertSpacesOnTabs(true);s.Editor.SetAutoIndentEnabled(true);s.Editor.SetShowLineNumbersEnabled(true);s.Editor.SetShowMatchingBrackets(true);s.Editor.SetText(script.Source);s.Synced=script.Source;s.Init=true;}else if(s.Synced!=script.Source){s.Editor.SetText(script.Source);s.Synced=script.Source;}return s;}
        struct KeyOption{const char*Name;int Key;};
        static constexpr KeyOption Keys[]={{"None",0},{"F1",GLFW_KEY_F1},{"F2",GLFW_KEY_F2},{"F3",GLFW_KEY_F3},{"F4",GLFW_KEY_F4},{"F5",GLFW_KEY_F5},{"F6",GLFW_KEY_F6},{"F7",GLFW_KEY_F7},{"F8",GLFW_KEY_F8},{"F9",GLFW_KEY_F9},{"F10",GLFW_KEY_F10},{"F11",GLFW_KEY_F11},{"F12",GLFW_KEY_F12},{"A",GLFW_KEY_A},{"B",GLFW_KEY_B},{"C",GLFW_KEY_C},{"D",GLFW_KEY_D},{"E",GLFW_KEY_E},{"F",GLFW_KEY_F},{"G",GLFW_KEY_G},{"H",GLFW_KEY_H},{"I",GLFW_KEY_I},{"J",GLFW_KEY_J},{"K",GLFW_KEY_K},{"L",GLFW_KEY_L},{"M",GLFW_KEY_M},{"N",GLFW_KEY_N},{"O",GLFW_KEY_O},{"P",GLFW_KEY_P},{"Q",GLFW_KEY_Q},{"R",GLFW_KEY_R},{"S",GLFW_KEY_S},{"T",GLFW_KEY_T},{"U",GLFW_KEY_U},{"V",GLFW_KEY_V},{"W",GLFW_KEY_W},{"X",GLFW_KEY_X},{"Y",GLFW_KEY_Y},{"Z",GLFW_KEY_Z}};
    }

    void JavaScriptPage::render(PageContext& context,PageManager& manager)
    {
        auto& engine=context.runtimeBindings;auto& settings=engine.scriptSettings();static std::string status;
        ImGui::TextWrapped("First-class QuickJS runtime automation. Workspace scripts run outside binding evaluation, so they can safely create/configure bindings, controls, banks, profiles, object models and pointers, then drive shader/runtime outputs.");
        if(ImGui::Button("+ Inline script")){auto&s=engine.addScript();s.Source="// q.graph.*, q.runtime.*, q.re.* and q.import() are available here.\n";}ImGui::SameLine();if(ImGui::Button("+ External script")){auto&s=engine.addScript();s.External=true;s.Path=(runtimeQuickJSScriptDirectory()/"script.js").string();}ImGui::SameLine();if(ImGui::Button("Reload all")){runtimeReloadAllWorkspaceScripts();status="all script contexts reloaded";}ImGui::SameLine();if(ImGui::Button("Save .d.ts")){std::string error;status=runtimeSaveQuickJSTypeDeclarations(error)?"saved "+runtimeQuickJSTypeDeclarationsPath().string():error;}ImGui::SameLine();if(ImGui::Button("Install Terraria/Astroflux v11 migration")){std::string error;status=runtimeInstallTerrariaAstrofluxMigration(engine,error)?"installed external migration script":error;}
        if(!status.empty())ImGui::TextDisabled("%s",status.c_str());
        bool changed=false;changed|=ImGui::Checkbox("External script hot reload",&settings.ExternalHotReload);ImGui::SameLine();ImGui::TextDisabled("polls main scripts + q.import() dependencies and rebuilds the context on change");
        ImGui::SeparatorText("Global reload hotkey");changed|=ImGui::Checkbox("Ctrl##jsReload",&settings.ReloadHotkeyCtrl);ImGui::SameLine();changed|=ImGui::Checkbox("Alt##jsReload",&settings.ReloadHotkeyAlt);ImGui::SameLine();changed|=ImGui::Checkbox("Shift##jsReload",&settings.ReloadHotkeyShift);ImGui::SameLine();const char*preview="None";for(const auto&k:Keys)if(k.Key==settings.ReloadHotkeyKey){preview=k.Name;break;}ImGui::SetNextItemWidth(100);if(ImGui::BeginCombo("Key##jsReload",preview)){for(const auto&k:Keys){const bool selected=k.Key==settings.ReloadHotkeyKey;if(ImGui::Selectable(k.Name,selected)){settings.ReloadHotkeyKey=k.Key;changed=true;}if(selected)ImGui::SetItemDefaultFocus();}ImGui::EndCombo();}ImGui::SameLine();ImGui::TextDisabled("uses evdev globally; GLFW fallback");if(changed)engine.markChanged();
        ImGui::SeparatorText("Workspace scripts");std::optional<std::size_t> erase;
        for(std::size_t i=0;i<engine.scripts().size();++i){auto&script=engine.scripts()[i];ImGui::PushID(static_cast<int>(script.Id&0x7fffffffULL));const std::string header=std::string(script.Name)+(script.Enabled?"":"  DISABLED")+"###RuntimeScript"+std::to_string(script.Id);if(ImGui::CollapsingHeader(header.c_str(),ImGuiTreeNodeFlags_DefaultOpen)){bool local=false;local|=ImGui::Checkbox("Enabled",&script.Enabled);ImGui::SameLine();ImGui::SetNextItemWidth(240);local|=ImGui::InputText("Name",script.Name,sizeof(script.Name));ImGui::SameLine();local|=ImGui::Checkbox("External",&script.External);ImGui::SameLine();if(ImGui::SmallButton("Reload")){runtimeResetWorkspaceScript(script.Id);++script.ReloadCount;}ImGui::SameLine();if(ImGui::SmallButton("Remove"))erase=i;
            ImGui::SetNextItemWidth(160);local|=ImGui::DragFloat("Update Hz",&script.UpdateHz,0.5f,0.5f,500.0f,"%.1f");ImGui::SameLine();ImGui::SetNextItemWidth(150);local|=ImGui::DragFloat("Timeout",&script.TimeoutMs,0.1f,0.1f,100.0f,"%.1f ms");ImGui::SameLine();local|=ImGui::Checkbox("Hot reload##script",&script.HotReload);ImGui::SetNextItemWidth(180);local|=ImGui::InputText("Group",script.Group,sizeof(script.Group));ImGui::SameLine();ImGui::SetNextItemWidth(80);local|=ImGui::InputInt("Order",&script.Order);
            if(script.External){char path[1024]{};std::snprintf(path,sizeof(path),"%s",script.Path.c_str());ImGui::SetNextItemWidth(-1);if(ImGui::InputText("Path",path,sizeof(path))){script.Path=path;local=true;}ImGui::TextDisabled("Relative paths resolve under %s",runtimeQuickJSScriptDirectory().string().c_str());}
            else{auto&e=editor(script);e.Editor.Render("##WorkspaceScriptEditor",ImVec2(-1,300));if(e.Editor.IsTextChanged()){script.Source=e.Editor.GetText();e.Synced=script.Source;runtimeResetWorkspaceScript(script.Id);local=true;}}
            ImGui::Text("Runs %llu  compiles %llu  reloads %llu  timeouts %llu  last %.3f ms",static_cast<unsigned long long>(script.RunCount),static_cast<unsigned long long>(script.CompileCount),static_cast<unsigned long long>(script.ReloadCount),static_cast<unsigned long long>(script.TimeoutCount),script.LastMilliseconds);if(!script.Status.empty())ImGui::TextWrapped("Status: %s",script.Status.c_str());if(!script.LastLog.empty())ImGui::TextWrapped("Log: %s",script.LastLog.c_str());if(!script.Dependencies.empty()&&ImGui::TreeNode("Imported dependencies")){for(const auto&dep:script.Dependencies)ImGui::BulletText("%s",dep.c_str());ImGui::TreePop();}if(local)engine.markChanged();}
            ImGui::Separator();ImGui::PopID();}
        if(erase)engine.eraseScript(*erase);
        ImGui::SeparatorText("Graph access");ImGui::TextWrapped("Scripts can configure the same graph exposed by the tabs: q.graph.ensureBinding/ensureControl/ensureBank/ensureObject/ensurePointer/ensureProfile, binding operations, enable/disable/remove nodes and profile activation. q.runtime drives shaders/materials/brightness/framebuffer state immediately. q.re exposes process memory/signatures/disassembly.");if(ImGui::Button("Open Bindings"))manager.open("bindings");ImGui::SameLine();if(ImGui::Button("Open Controls"))manager.open("controls");ImGui::SameLine();if(ImGui::Button("Open Profiles"))manager.open("profiles");
    }
}
''')

# Register page.
p="src/ui/PageManager.cpp";text=read(p);text=replace_once(text,'#include "quartz/client/ui/pages/BindingsPage.hpp"','#include "quartz/client/ui/pages/BindingsPage.hpp"\n#include "quartz/client/ui/pages/JavaScriptPage.hpp"',"js page include");text=replace_once(text,'        manager.add<BindingsPage>();','        manager.add<JavaScriptPage>();\n        manager.add<BindingsPage>();',"js page register");write(p,text)

# Profile UI: include scripts as members and correct description.
p="src/ui/RuntimeUI.cpp";text=read(p);text=replace_once(text,'ImGui::SameLine(); ImGui::TextDisabled("Profiles mass-enable/disable graph nodes and can be activated with a key combination while Quartz has keyboard focus.");','ImGui::SameLine(); ImGui::TextDisabled("Profiles mass-enable/disable bindings, controls and JavaScript workspace scripts. Hotkeys use evdev globally when available.");',"profile description");text=replace_once(text,'if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exclusive profiles disable every binding/control first, then enable their selected members.");','if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exclusive profiles disable every binding/control/script first, then enable their selected members.");',"profile exclusive tooltip");control_table_end='''                    ImGui::EndTable();\n                }\n                if (changed) engine.markChanged();'''.replace('\\n','\n');script_table=r'''                    ImGui::EndTable();
                }
                ImGui::SeparatorText("JavaScript scripts");
                if (ImGui::BeginTable("ProfileScripts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 65.0f); ImGui::TableSetupColumn("Script"); ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
                    for (const auto& script : engine.scripts()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); bool member = std::find(profile.ScriptIds.begin(), profile.ScriptIds.end(), script.Id) != profile.ScriptIds.end(); if (ImGui::Checkbox(("##ps" + std::to_string(script.Id)).c_str(), &member)) { if (member) profile.ScriptIds.push_back(script.Id); else std::erase(profile.ScriptIds, script.Id); changed = true; } ImGui::TableNextColumn(); ImGui::TextUnformatted(script.Name); ImGui::TableNextColumn(); ImGui::TextColored(script.Enabled ? ImVec4(0.2f,0.8f,0.3f,1) : ImVec4(0.8f,0.25f,0.25f,1), "%s", script.Enabled ? "enabled" : "disabled"); }
                    ImGui::EndTable();
                }
                if (changed) engine.markChanged();''';text=replace_once(text,control_table_end,script_table,"profile script table");write(p,text)

# Application: reload hotkey + workspace scripts after graph evaluation, merge outputs.
p="src/Application.cpp";text=read(p);text=replace_once(text,'        runtimeBindings.pollProfileHotkeys(window.handle(), keyboardInput);','        runtimeBindings.pollProfileHotkeys(window.handle(), keyboardInput);\n        runtimeBindings.pollScriptReloadHotkey(window.handle(), keyboardInput);',"script reload hotkey app");old='''            runtimeBindings.update(runtimeContext, shaderFramebuffer);\n            const RuntimeControlOutput controlOutput = runtimeBindings.evaluateControls(shaderFramebuffer);'''.replace('\\n','\n');new='''            runtimeBindings.update(runtimeContext, shaderFramebuffer);
            RuntimeControlOutput controlOutput = runtimeBindings.evaluateControls(shaderFramebuffer);
            const RuntimeControlOutput scriptOutput = runtimeEvaluateWorkspaceScripts(runtimeBindings, runtimeContext, shaderFramebuffer);
            if (scriptOutput.ShaderPresetIndex) controlOutput.ShaderPresetIndex = scriptOutput.ShaderPresetIndex;
            if (scriptOutput.ShaderId) controlOutput.ShaderId = scriptOutput.ShaderId;
            if (scriptOutput.GlobalBrightness) controlOutput.GlobalBrightness = scriptOutput.GlobalBrightness;
            if (scriptOutput.SendFramebuffer) controlOutput.SendFramebuffer = scriptOutput.SendFramebuffer;
            if (scriptOutput.BaseColorMode) controlOutput.BaseColorMode = scriptOutput.BaseColorMode;
            if (scriptOutput.ShaderPresetIndex || scriptOutput.ShaderId) controlOutput.ShaderTransitionSeconds = scriptOutput.ShaderTransitionSeconds;''';text=replace_once(text,old,new,"workspace evaluate app");write(p,text)

# Extend d.ts by replacing the declarations tail in QuickJSApi.cpp.
p="src/runtime/QuickJSApi.cpp";text=read(p);old='''    readonly re: QuartzReverseEngineeringAPI;\n    readonly time: number;''';new='''    readonly re: QuartzReverseEngineeringAPI;
    /** CommonJS-like local .js/.mjs import. Relative paths resolve beside the current script/import. */
    import(path: string): any;
    readonly graph: QuartzGraphAPI;
    readonly runtime: QuartzRuntimeOutputAPI;
    readonly time: number;''';text=replace_once(text,old,new,"dts q extensions");insert_before='''interface QuartzBindingAPI {''';defs=r'''interface QuartzBindingConfig { [key: string]: any; }
interface QuartzControlConfig { [key: string]: any; }
interface QuartzGraphAPI {
    ensureBinding(name: string, config?: QuartzBindingConfig): bigint;
    ensureControl(name: string, config?: QuartzControlConfig): bigint;
    ensureBank(name: string, config?: Record<string, any>): bigint;
    setBank(idOrName: QuartzId, value: number | bigint | boolean | string): boolean;
    ensureObject(name: string, config?: Record<string, any>): bigint;
    ensurePointer(name: string, config?: Record<string, any>): bigint;
    ensureProfile(name: string, config?: { enabled?: boolean; exclusive?: boolean; ctrl?: boolean; alt?: boolean; shift?: boolean; key?: number; bindings?: QuartzId[]; controls?: QuartzId[]; scripts?: QuartzId[] }): bigint;
    applyProfile(idOrName: QuartzId): boolean;
    activeProfile(): string | undefined;
    bindingOperation(idOrName: QuartzId, operation: "Refresh" | "ForceUpdate" | "Invalidate" | "ResetState" | "RetryRegisterCapture" | "RescanPattern" | "RebindProcess" | "ClearError"): boolean;
    setBindingEnabled(idOrName: QuartzId, enabled: boolean): boolean;
    setControlEnabled(idOrName: QuartzId, enabled: boolean): boolean;
    setScriptEnabled(idOrName: QuartzId, enabled: boolean): boolean;
    removeBinding(idOrName: QuartzId): boolean; removeControl(idOrName: QuartzId): boolean; removeBank(idOrName: QuartzId): boolean; removeProfile(idOrName: QuartzId): boolean; removePointer(idOrName: QuartzId): boolean; removeObject(idOrName: QuartzId): boolean;
    save(): boolean;
}
interface QuartzRuntimeOutputAPI {
    shader(id: string, transitionSeconds?: number): boolean;
    shaderPreset(index: number, transitionSeconds?: number): boolean;
    brightness(value: number): boolean;
    sendFramebuffer(enabled: boolean): boolean;
    baseColorMode(mode: number): boolean;
    material(id: string, component: number, value: number): boolean;
    currentShader(): string | undefined;
    previousShader(): string | undefined;
}

interface QuartzBindingAPI {''';text=replace_once(text,insert_before,defs,"dts graph api");write(p,text)

# README note.
p="README.md";text=read(p);anchor="- embedded QuickJS binding source";text=text.replace(anchor,"- first-class JavaScript workspace with embedded QuickJS: graph mutation, runtime outputs, q.re low-level tooling, external script hot reload, q.import dependency watching, evdev reload hotkeys, profile-controlled scripts, plus per-binding QuickJS sources",1) if anchor in text else text+"\n- First-class QuickJS JavaScript workspace with external hot reload/imports and full runtime graph control.\n";write(p,text)

print("JavaScript workspace migration applied")
