from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def read(path): return (ROOT / path).read_text()
def write(path, text): (ROOT / path).write_text(text)
def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1: raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

# RuntimeScript gets script-owned persistent state and an explicit legacy capability gate.
path = "include/quartz/client/runtime/RuntimeTypes.hpp"
text = read(path)
text = replace_once(text,
'''        std::string Source = "// Quartz runtime script\\n";
        bool HotReload = true;
''',
'''        std::string Source = "// Quartz runtime script\\n";
        std::string PersistentStateJson = "{}";
        bool HotReload = true;
        bool LegacyBridge = false;
''', "RuntimeScript fields")
write(path, text)

# Forward declarations + public UI signatures.
path = "include/quartz/client/Forward.hpp"
text = read(path)
text = replace_once(text, "    class RuntimeBindingEngine;\n", "    class RuntimeBindingEngine;\n    class JavaScriptRuntime;\n", "JavaScriptRuntime forward declaration")
write(path, text)

path = "include/quartz/client/Functions.hpp"
text = read(path)
text = replace_once(text, "    void drawRuntimeProfiles(RuntimeBindingEngine& engine);", "    void drawRuntimeProfiles(RuntimeBindingEngine& engine, JavaScriptRuntime& javascript);", "profile UI signature")
old = "RuntimeBindingEngine& runtimeBindings, RuntimeTelemetry& runtimeTelemetry"
new = "RuntimeBindingEngine& runtimeBindings, JavaScriptRuntime& javascript, RuntimeTelemetry& runtimeTelemetry"
text = replace_once(text, old, new, "drawUi declaration")
write(path, text)

# UI context construction follows the new signature.
path = "src/ui/UI.cpp"
text = read(path)
text = replace_once(text, old, new, "drawUi definition")
text = replace_once(text, "deviceState, runtimeBindings, runtimeTelemetry, autoGain", "deviceState, runtimeBindings, javascript, runtimeTelemetry, autoGain", "PageContext initializer")
write(path, text)

# Profiles keep legacy membership in the old graph file but select scripts from the real JavaScript runtime.
path = "src/ui/RuntimeUI.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/runtime/QuickJS.hpp"\n', '#include "quartz/client/runtime/QuickJS.hpp"\n#include "quartz/client/runtime/JavaScriptRuntime.hpp"\n', "RuntimeUI JavaScriptRuntime include")
text = replace_once(text, "    void drawRuntimeProfiles(RuntimeBindingEngine& engine)\n", "    void drawRuntimeProfiles(RuntimeBindingEngine& engine, JavaScriptRuntime& javascript)\n", "drawRuntimeProfiles definition")
text = replace_once(text, 'ImGui::TextUnformatted("Binding profiles");', 'ImGui::TextUnformatted("Profiles");', "profiles heading")
text = replace_once(text, 'ImGui::TextDisabled("Profiles mass-enable/disable bindings, controls and JavaScript workspace scripts. Hotkeys use evdev globally when available.");', 'ImGui::TextDisabled("Profiles can still group deprecated graph nodes and explicitly select first-class JavaScript scripts. Hotkeys use evdev globally when available.");', "profiles description")
text = text.replace("engine.scripts()", "javascript.scripts()")
write(path, text)

# Deprecation language in the old graph pages.
path = "src/ui/pages/BindingsPage.cpp"
text = read(path)
text = replace_once(text,
'        ImGui::TextWrapped("Bindings are the data nodes of the runtime graph. Pick a source, optionally compare/aggregate/transform it, then route the result to materials, actions, controls or the value bank.");',
'        ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.28f, 1.0f), "Deprecated runtime graph feature");\n        ImGui::TextWrapped("Bindings are retained for old visual graphs. New automation should use first-class JavaScript APIs such as q.process, q.memory, q.signature, q.events and q.runtime instead.");', "bindings deprecation copy")
write(path, text)

path = "src/ui/pages/ControlsPage.cpp"
text = read(path)
text = replace_once(text,
'        ImGui::TextWrapped("Controls turn binding values into conditions and actions. Configure the input/condition first, then the target and any extra actions.");',
'        ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.28f, 1.0f), "Deprecated runtime graph feature");\n        ImGui::TextWrapped("Controls remain for existing graphs. New state machines should use q.state/q.storage, q.events and q.runtime directly from JavaScript.");', "controls deprecation copy")
write(path, text)

path = "src/runtime/Runtime.cpp"
text = read(path)
text = replace_once(text, 'case RuntimeSourceKind::Script: return "QuickJS script";', 'case RuntimeSourceKind::Script: return "QuickJS script (deprecated binding source)";', "script binding label")
write(path, text)

# Low-level API: keep q.re as a compatibility alias, but make semantic namespaces primary.
path = "src/runtime/QuickJSApi.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/native/NativeDisassembly.hpp"\n', '#include "quartz/client/native/NativeDisassembly.hpp"\n#include "quartz/client/native/ExecutionProbe.hpp"\n#include "quartz/client/input/Input.hpp"\n', "QuickJS low-level includes")
marker = "    }\n\n    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api)"
helpers = r'''        JSValue processObject(JSContext* ctx, const RuntimeProcessInfo& process)
        {
            JSValue object = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, object, "pid", JS_NewInt32(ctx, process.Pid));
            JS_SetPropertyStr(ctx, object, "name", JS_NewString(ctx, process.Name.c_str()));
            JS_SetPropertyStr(ctx, object, "exe", JS_NewString(ctx, process.Exe.c_str()));
            JS_SetPropertyStr(ctx, object, "title", JS_NewString(ctx, process.Title.c_str()));
            JS_SetPropertyStr(ctx, object, "commandLine", JS_NewString(ctx, process.CommandLine.c_str()));
            return object;
        }

        JSValue jsFindProcess(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            if (argc < 1) return JS_UNDEFINED;
            const auto processes = enumerateRuntimeProcesses();
            if (JS_IsNumber(argv[0]) || JS_IsBigInt(ctx, argv[0]))
            {
                pid_t pid = 0; if (!valueToPid(ctx, argv[0], pid)) return JS_UNDEFINED;
                const auto it = std::ranges::find(processes, pid, &RuntimeProcessInfo::Pid);
                return it == processes.end() ? JS_UNDEFINED : processObject(ctx, *it);
            }
            const char* raw = JS_ToCString(ctx, argv[0]); if (!raw) return JS_EXCEPTION;
            const std::string query = runtimeLower(raw); JS_FreeCString(ctx, raw);
            const RuntimeProcessInfo* best = nullptr; int bestScore = -1;
            for (const auto& process : processes)
            {
                const std::string name = runtimeLower(process.Name), exe = runtimeLower(process.Exe), title = runtimeLower(process.Title), command = runtimeLower(process.CommandLine);
                const std::string filename = runtimeLower(std::filesystem::path(process.Exe).filename().string());
                int score = -1;
                if (name == query || filename == query) score = 100;
                else if (exe == query || title == query) score = 90;
                else if (name.find(query) != std::string::npos || filename.find(query) != std::string::npos) score = 70;
                else if (exe.find(query) != std::string::npos || title.find(query) != std::string::npos) score = 60;
                else if (command.find(query) != std::string::npos) score = 40;
                if (score > bestScore || (score == bestScore && best && process.Pid > best->Pid)) { best = &process; bestScore = score; }
            }
            return best ? processObject(ctx, *best) : JS_UNDEFINED;
        }

        bool inputKey(JSContext* ctx, JSValueConst value, std::uint16_t& key)
        {
            if (JS_IsNumber(value))
            {
                std::uint32_t raw = 0; if (JS_ToUint32(ctx, &raw, value) < 0 || raw > KEY_MAX) return false; key = static_cast<std::uint16_t>(raw); return true;
            }
            const char* raw = JS_ToCString(ctx, value); if (!raw) return false;
            std::string name(raw); JS_FreeCString(ctx, raw); std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (name.starts_with("KEY_")) name.erase(0, 4);
            if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') { key = static_cast<std::uint16_t>(KEY_A + (name[0] - 'A')); return true; }
            if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') { key = static_cast<std::uint16_t>(KEY_1 + (name[0] - '1')); return true; }
            if (name == "0") { key = KEY_0; return true; }
            if (name.size() >= 2 && name[0] == 'F') { int number = 0; const auto [ptr, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), number); if (ec == std::errc{} && ptr == name.data() + name.size() && number >= 1 && number <= 10) { key = static_cast<std::uint16_t>(KEY_F1 + number - 1); return true; } if (number == 11) { key = KEY_F11; return true; } if (number == 12) { key = KEY_F12; return true; } }
            static constexpr std::pair<std::string_view, std::uint16_t> Names[] = {{"ESC",KEY_ESC},{"ESCAPE",KEY_ESC},{"SPACE",KEY_SPACE},{"ENTER",KEY_ENTER},{"TAB",KEY_TAB},{"BACKSPACE",KEY_BACKSPACE},{"UP",KEY_UP},{"DOWN",KEY_DOWN},{"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},{"HOME",KEY_HOME},{"END",KEY_END},{"PAGEUP",KEY_PAGEUP},{"PAGEDOWN",KEY_PAGEDOWN},{"INSERT",KEY_INSERT},{"DELETE",KEY_DELETE},{"CAPSLOCK",KEY_CAPSLOCK},{"SCROLLLOCK",KEY_SCROLLLOCK},{"PAUSE",KEY_PAUSE},{"LEFTCTRL",KEY_LEFTCTRL},{"RIGHTCTRL",KEY_RIGHTCTRL},{"LEFTALT",KEY_LEFTALT},{"RIGHTALT",KEY_RIGHTALT},{"LEFTSHIFT",KEY_LEFTSHIFT},{"RIGHTSHIFT",KEY_RIGHTSHIFT}};
            for (const auto& [candidate, code] : Names) if (name == candidate) { key = code; return true; }
            return false;
        }

        bool boolObjectProperty(JSContext* ctx, JSValueConst object, const char* name, bool& output)
        {
            if (!JS_IsObject(object)) return false; JSValue value = JS_GetPropertyStr(ctx, object, name); if (JS_IsUndefined(value)) { JS_FreeValue(ctx, value); return false; } const int result = JS_ToBool(ctx, value); JS_FreeValue(ctx, value); if (result < 0) return false; output = result != 0; return true;
        }

        JSValue jsInputKeyDown(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* state = scriptContext(ctx); std::uint16_t key = 0; if (!state || !state->Keyboard || argc < 1 || !inputKey(ctx, argv[0], key)) return JS_FALSE; return JS_NewBool(ctx, state->Keyboard->shortcutDown(key, false, false, false));
        }

        JSValue jsInputShortcut(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* state = scriptContext(ctx); std::uint16_t key = 0; if (!state || !state->Keyboard || argc < 1 || !inputKey(ctx, argv[0], key)) return JS_FALSE;
            bool ctrl = false, alt = false, shift = false; if (argc > 1 && JS_IsObject(argv[1])) { boolObjectProperty(ctx, argv[1], "ctrl", ctrl); boolObjectProperty(ctx, argv[1], "alt", alt); boolObjectProperty(ctx, argv[1], "shift", shift); }
            return JS_NewBool(ctx, state->Keyboard->shortcutDown(key, ctrl, alt, shift));
        }

        JSValue jsCapsLock(JSContext* ctx, JSValueConst, int, JSValueConst*) { const auto* state = scriptContext(ctx); return JS_NewBool(ctx, state && state->SignalContext && state->SignalContext->Keys.CapsLockActive); }
        JSValue jsScrollLock(JSContext* ctx, JSValueConst, int, JSValueConst*) { const auto* state = scriptContext(ctx); return JS_NewBool(ctx, state && state->SignalContext && state->SignalContext->Keys.ScrollLockActive); }

        JSValue registerSnapshot(JSContext* ctx, const user_regs_struct& regs)
        {
            JSValue object = JS_NewObject(ctx);
#define QUARTZ_REG(name) JS_SetPropertyStr(ctx, object, #name, JS_NewBigUint64(ctx, static_cast<std::uint64_t>(regs.name)))
            QUARTZ_REG(rax); QUARTZ_REG(rbx); QUARTZ_REG(rcx); QUARTZ_REG(rdx); QUARTZ_REG(rsi); QUARTZ_REG(rdi); QUARTZ_REG(rbp); QUARTZ_REG(rsp); QUARTZ_REG(r8); QUARTZ_REG(r9); QUARTZ_REG(r10); QUARTZ_REG(r11); QUARTZ_REG(r12); QUARTZ_REG(r13); QUARTZ_REG(r14); QUARTZ_REG(r15); QUARTZ_REG(rip); QUARTZ_REG(eflags); QUARTZ_REG(orig_rax); QUARTZ_REG(fs_base); QUARTZ_REG(gs_base);
#undef QUARTZ_REG
            return object;
        }

        JSValue jsBreakpointArm(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            pid_t pid = 0; std::uintptr_t address = 0; if (argc < 2 || !valueToPid(ctx, argv[0], pid) || !valueToAddress(ctx, argv[1], address)) return JS_ThrowTypeError(ctx, "q.breakpoint.arm(pid, address): invalid pid/address"); std::string error; if (!executionProbe().start(pid, address, error)) return jsError(ctx, error); return JS_TRUE;
        }
        JSValue jsBreakpointCancel(JSContext*, JSValueConst, int, JSValueConst*) { executionProbe().stop(); return JS_TRUE; }
        JSValue jsBreakpointRunning(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewBool(ctx, executionProbe().running()); }
        JSValue jsBreakpointHit(JSContext* ctx, JSValueConst, int, JSValueConst*)
        {
            const auto hit = executionProbe().hit(); if (!hit) return JS_UNDEFINED; JSValue object = JS_NewObject(ctx); JS_SetPropertyStr(ctx, object, "time", JS_NewFloat64(ctx, hit->Time)); JS_SetPropertyStr(ctx, object, "pid", JS_NewInt32(ctx, hit->Pid)); JS_SetPropertyStr(ctx, object, "tid", JS_NewInt32(ctx, hit->Tid)); JS_SetPropertyStr(ctx, object, "address", addressValue(ctx, hit->Address)); if (hit->HasRegisters) JS_SetPropertyStr(ctx, object, "registers", registerSnapshot(ctx, hit->Registers)); return object;
        }
'''
text = replace_once(text, marker, helpers + "    }\n\n    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api)", "insert first-class low-level helpers")
start = text.index("    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api)")
replacement = r'''    void runtimeInstallQuickJSLowLevelApi(JSContext* ctx, JSValueConst api)
    {
        JS_SetPropertyStr(ctx, api, "loop", JS_NewCFunction(ctx, jsLoop, "loop", 2));

        JSValue process = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, process, "list", JS_NewCFunction(ctx, jsProcesses, "list", 0));
        JS_SetPropertyStr(ctx, process, "find", JS_NewCFunction(ctx, jsFindProcess, "find", 1));
        JS_SetPropertyStr(ctx, process, "alive", JS_NewCFunction(ctx, jsProcessAlive, "alive", 1));
        JS_SetPropertyStr(ctx, process, "modules", JS_NewCFunction(ctx, jsModules, "modules", 1));
        JS_SetPropertyStr(ctx, process, "regions", JS_NewCFunction(ctx, jsRegions, "regions", 1));
        JS_SetPropertyStr(ctx, api, "process", process);

        JSValue memory = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, memory, "read", JS_NewCFunction(ctx, jsRead, "read", 3));
        JS_SetPropertyStr(ctx, memory, "write", JS_NewCFunction(ctx, jsWrite, "write", 4));
        JS_SetPropertyStr(ctx, memory, "readBytes", JS_NewCFunction(ctx, jsReadBytes, "readBytes", 3));
        JS_SetPropertyStr(ctx, memory, "writeBytes", JS_NewCFunction(ctx, jsWriteBytes, "writeBytes", 3));
        JS_SetPropertyStr(ctx, memory, "loop", JS_NewCFunction(ctx, jsAddressLoop, "loop", 4));
        JS_SetPropertyStr(ctx, api, "memory", memory);

        JSValue signature = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, signature, "find", JS_NewCFunction(ctx, jsSignature, "find", 3));
        JS_SetPropertyStr(ctx, api, "signature", signature);

        JSValue disassembly = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, disassembly, "decode", JS_NewCFunction(ctx, jsDisassemble, "decode", 3));
        JS_SetPropertyStr(ctx, api, "disassembly", disassembly);

        JSValue breakpoint = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, breakpoint, "arm", JS_NewCFunction(ctx, jsBreakpointArm, "arm", 2));
        JS_SetPropertyStr(ctx, breakpoint, "cancel", JS_NewCFunction(ctx, jsBreakpointCancel, "cancel", 0));
        JS_SetPropertyStr(ctx, breakpoint, "running", JS_NewCFunction(ctx, jsBreakpointRunning, "running", 0));
        JS_SetPropertyStr(ctx, breakpoint, "hit", JS_NewCFunction(ctx, jsBreakpointHit, "hit", 0));
        JS_SetPropertyStr(ctx, api, "breakpoint", breakpoint);

        JSValue input = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, input, "keyDown", JS_NewCFunction(ctx, jsInputKeyDown, "keyDown", 1));
        JS_SetPropertyStr(ctx, input, "shortcut", JS_NewCFunction(ctx, jsInputShortcut, "shortcut", 2));
        JS_SetPropertyStr(ctx, input, "capsLock", JS_NewCFunction(ctx, jsCapsLock, "capsLock", 0));
        JS_SetPropertyStr(ctx, input, "scrollLock", JS_NewCFunction(ctx, jsScrollLock, "scrollLock", 0));
        JS_SetPropertyStr(ctx, api, "input", input);

        // Compatibility alias for scripts written before the namespace split.
        JSValue re = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, re, "processes", JS_NewCFunction(ctx, jsProcesses, "processes", 0)); JS_SetPropertyStr(ctx, re, "modules", JS_NewCFunction(ctx, jsModules, "modules", 1)); JS_SetPropertyStr(ctx, re, "regions", JS_NewCFunction(ctx, jsRegions, "regions", 1)); JS_SetPropertyStr(ctx, re, "processAlive", JS_NewCFunction(ctx, jsProcessAlive, "processAlive", 1));
        JS_SetPropertyStr(ctx, re, "read", JS_NewCFunction(ctx, jsRead, "read", 3)); JS_SetPropertyStr(ctx, re, "write", JS_NewCFunction(ctx, jsWrite, "write", 4)); JS_SetPropertyStr(ctx, re, "readBytes", JS_NewCFunction(ctx, jsReadBytes, "readBytes", 3)); JS_SetPropertyStr(ctx, re, "writeBytes", JS_NewCFunction(ctx, jsWriteBytes, "writeBytes", 3)); JS_SetPropertyStr(ctx, re, "signature", JS_NewCFunction(ctx, jsSignature, "signature", 3)); JS_SetPropertyStr(ctx, re, "disassemble", JS_NewCFunction(ctx, jsDisassemble, "disassemble", 3)); JS_SetPropertyStr(ctx, re, "loop", JS_NewCFunction(ctx, jsAddressLoop, "loop", 4));
        JS_SetPropertyStr(ctx, api, "re", re);
    }

    std::string_view runtimeQuickJSTypeDeclarations() noexcept
    {
        static constexpr std::string_view Declarations = R"TS(// Quartz first-class JavaScript runtime API
// Generated by Quartz Client. Workspace scripts are function bodies with `q` available.

type QuartzId = number | bigint | string;
type QuartzAddress = bigint;
type QuartzInputKey = number | string;
type QuartzScalarType = "u8" | "i8" | "u16" | "i16" | "u32" | "i32" | "u64" | "i64" | "f32" | "float" | "f64" | "double" | "bool" | "boolean" | "ptr" | "pointer" | "address";

type QuartzEventName = "tick" | "shader.changed" | "key.down" | "key.up" | "key.changed" | "lock.changed" | "process.started" | "process.stopped" | "breakpoint.hit" | "script.loaded" | "script.reload" | string;
type QuartzEventHandle = bigint;

interface QuartzProcessInfo { pid: number; name: string; exe: string; title: string; commandLine: string; }
interface QuartzModuleInfo { base: QuartzAddress; end: QuartzAddress; size: bigint; name: string; path: string; }
interface QuartzRegionInfo { base: QuartzAddress; end: QuartzAddress; size: bigint; readable: boolean; writable: boolean; executable: boolean; path: string; }
interface QuartzInstruction { address: QuartzAddress; size: number; text: string; bytes: string; }
interface QuartzRegisters { [name: string]: bigint; }
interface QuartzBreakpointHit { time: number; pid: number; tid: number; address: QuartzAddress; registers?: QuartzRegisters; }

interface QuartzProcessAPI {
    list(): QuartzProcessInfo[];
    /** Finds by pid, exact name/executable, or fuzzy process text. Newest pid wins ties. */
    find(query: number | bigint | string): QuartzProcessInfo | undefined;
    alive(pid: number): boolean;
    modules(pid: number): QuartzModuleInfo[];
    regions(pid: number): QuartzRegionInfo[];
}
interface QuartzMemoryAPI {
    read(pid: number, address: QuartzAddress | number, type?: QuartzScalarType): number | bigint | boolean;
    write(pid: number, address: QuartzAddress | number, type: QuartzScalarType, value: number | bigint | boolean): true;
    readBytes(pid: number, address: QuartzAddress | number, length: number): number[];
    writeBytes(pid: number, address: QuartzAddress | number, bytes: number[] | string): true;
    loop(start: QuartzAddress | number, count: number, stride: number | bigint, callback: (address: QuartzAddress, index: number) => void | boolean): number;
}
interface QuartzSignatureAPI { find(pid: number, pattern: string, executableOnly?: boolean): QuartzAddress | undefined; }
interface QuartzDisassemblyAPI { decode(pid: number, address: QuartzAddress | number, count?: number): QuartzInstruction[]; }
interface QuartzBreakpointAPI {
    /** Arms the same one-shot execution probe used by the RE UI. */
    arm(pid: number, address: QuartzAddress | number): true;
    cancel(): true;
    running(): boolean;
    hit(): QuartzBreakpointHit | undefined;
}
interface QuartzInputAPI {
    /** Accepts Linux evdev numeric KEY_* codes or common names such as "F8", "A", "SPACE". */
    keyDown(key: QuartzInputKey): boolean;
    shortcut(key: QuartzInputKey, modifiers?: { ctrl?: boolean; alt?: boolean; shift?: boolean }): boolean;
    capsLock(): boolean;
    scrollLock(): boolean;
}
interface QuartzEventsAPI {
    subscribe(type: QuartzEventName | "*", callback: (event: any) => void): QuartzEventHandle;
    unsubscribe(handle: QuartzEventHandle | number): boolean;
    emit(type: string, data?: any): true;
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
    /** Releases a sticky JS override: all, shader, brightness, framebuffer, or baseColorMode. */
    clear(target?: "all" | "shader" | "brightness" | "framebuffer" | "sendFramebuffer" | "baseColor" | "baseColorMode"): boolean;
}

interface QuartzBindingConfig { [key: string]: any; }
interface QuartzControlConfig { [key: string]: any; }
/** @deprecated Legacy visual binding graph. Prefer direct q.process/q.memory/q.events/q.runtime scripting. */
interface QuartzGraphAPI {
    ensureBinding(name: string, config?: QuartzBindingConfig): bigint;
    ensureControl(name: string, config?: QuartzControlConfig): bigint;
    ensureBank(name: string, config?: Record<string, any>): bigint;
    setBank(idOrName: QuartzId, value: number | bigint | boolean | string): boolean;
    ensureObject(name: string, config?: Record<string, any>): bigint;
    ensurePointer(name: string, config?: Record<string, any>): bigint;
    ensureProfile(name: string, config?: Record<string, any>): bigint;
    applyProfile(idOrName: QuartzId): boolean;
    activeProfile(): string | undefined;
    bindingOperation(idOrName: QuartzId, operation: string): boolean;
    setBindingEnabled(idOrName: QuartzId, enabled: boolean): boolean;
    setControlEnabled(idOrName: QuartzId, enabled: boolean): boolean;
    removeBinding(idOrName: QuartzId): boolean; removeControl(idOrName: QuartzId): boolean; removeBank(idOrName: QuartzId): boolean; removeProfile(idOrName: QuartzId): boolean; removePointer(idOrName: QuartzId): boolean; removeObject(idOrName: QuartzId): boolean;
    save(): boolean;
}
/** @deprecated Compatibility view of the old binding/control/value-bank runtime. Enabled per script. */
interface QuartzLegacyAPI {
    binding(idOrName: QuartzId): number | undefined;
    raw(idOrName: QuartzId): number | undefined;
    text(idOrName: QuartzId): string | undefined;
    address(idOrName: QuartzId): QuartzAddress | undefined;
    bank(idOrName: QuartzId): number | bigint | boolean | string | undefined;
    control(idOrName: QuartzId): boolean | undefined;
    triggered(idOrName: QuartzId): boolean | undefined;
    readonly graph: QuartzGraphAPI;
}
/** @deprecated Old monolithic reverse-engineering namespace. Use q.process/q.memory/q.signature/q.disassembly. */
interface QuartzReverseEngineeringAPI {
    processes(): QuartzProcessInfo[]; modules(pid: number): QuartzModuleInfo[]; regions(pid: number): QuartzRegionInfo[]; processAlive(pid: number): boolean;
    read(pid: number, address: QuartzAddress | number, type?: QuartzScalarType): number | bigint | boolean; write(pid: number, address: QuartzAddress | number, type: QuartzScalarType, value: number | bigint | boolean): true;
    readBytes(pid: number, address: QuartzAddress | number, length: number): number[]; writeBytes(pid: number, address: QuartzAddress | number, bytes: number[] | string): true;
    signature(pid: number, pattern: string, executableOnly?: boolean): QuartzAddress | undefined; disassemble(pid: number, address: QuartzAddress | number, count?: number): QuartzInstruction[];
    loop(start: QuartzAddress | number, count: number, stride: number | bigint, callback: (address: QuartzAddress, index: number) => void | boolean): number;
}

interface QuartzRuntimeAPI {
    readonly process: QuartzProcessAPI;
    readonly memory: QuartzMemoryAPI;
    readonly signature: QuartzSignatureAPI;
    readonly disassembly: QuartzDisassemblyAPI;
    readonly breakpoint: QuartzBreakpointAPI;
    readonly input: QuartzInputAPI;
    readonly events: QuartzEventsAPI;
    readonly runtime: QuartzRuntimeOutputAPI;
    /** Ephemeral state that lives until this script context reloads/stops. */
    state: Record<string, any>;
    /** Persistent JSON-only state saved independently of bindings/value-bank. */
    storage: Record<string, any>;
    import(path: string): any;
    log(...values: unknown[]): void;
    loop(count: number, callback: (index: number) => void | boolean): number;
    readonly time: number;
    readonly deltaTime: number;
    readonly id: bigint;
    readonly name: string;
    /** @deprecated Enable the per-script legacy bridge before using this. */ readonly legacy?: QuartzLegacyAPI;
    /** @deprecated Use q.process/q.memory/q.signature/q.disassembly. */ readonly re: QuartzReverseEngineeringAPI;
    /** @deprecated Enable the per-script legacy bridge and use q.legacy.graph if absolutely necessary. */ readonly graph?: QuartzGraphAPI;
    /** @deprecated Use first-class JS state/APIs. Requires the legacy bridge. */ binding?(idOrName: QuartzId): number | undefined;
    /** @deprecated Requires the legacy bridge. */ raw?(idOrName: QuartzId): number | undefined;
    /** @deprecated Requires the legacy bridge. */ text?(idOrName: QuartzId): string | undefined;
    /** @deprecated Requires the legacy bridge. */ address?(idOrName: QuartzId): QuartzAddress | undefined;
    /** @deprecated Use q.storage. Requires the legacy bridge. */ bank?(idOrName: QuartzId): number | bigint | boolean | string | undefined;
    /** @deprecated Use q.events/q.state. Requires the legacy bridge. */ control?(idOrName: QuartzId): boolean | undefined;
    /** @deprecated Use q.events. Requires the legacy bridge. */ triggered?(idOrName: QuartzId): boolean | undefined;
}

declare const q: QuartzRuntimeAPI;
)TS";
        return Declarations;
    }

    std::filesystem::path runtimeQuickJSTypeDeclarationsPath() { return settingsPath().parent_path() / "quartz-runtime.d.ts"; }

    bool runtimeSaveQuickJSTypeDeclarations(std::string& error)
    {
        const auto path = runtimeQuickJSTypeDeclarationsPath();
        std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) { error = "could not open " + path.string(); return false; }
        const auto declarations = runtimeQuickJSTypeDeclarations(); file.write(declarations.data(), static_cast<std::streamsize>(declarations.size()));
        if (!file) { error = "could not write " + path.string(); return false; }
        error.clear(); return true;
    }
}
'''
text = text[:start] + replacement
write(path, text)

# Runtime output remains first-class; q.graph itself is removed per script unless LegacyBridge is enabled.
path = "src/runtime/QuickJSGraphApi.cpp"
text = read(path)
anchor = '        JSValue runtimePreviousShader(JSContext*ctx,JSValueConst,int,JSValueConst*){auto*s=state(ctx);return s&&s->SignalContext?JS_NewString(ctx,s->SignalContext->PreviousShaderId.c_str()):JS_UNDEFINED;}\n'
clear_fn = r'''        JSValue runtimeClear(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* s = state(ctx); if (!s || !s->Output) return JS_FALSE;
            std::string target = "all"; if (argc > 0 && !JS_IsUndefined(argv[0]) && !toString(ctx, argv[0], target)) return JS_EXCEPTION; target = normalized(std::move(target));
            if (target == "all") *s->Output = {};
            else if (target == "shader") { s->Output->ShaderId.reset(); s->Output->ShaderPresetIndex.reset(); }
            else if (target == "brightness") s->Output->GlobalBrightness.reset();
            else if (target == "sendframebuffer" || target == "framebuffer") s->Output->SendFramebuffer.reset();
            else if (target == "basecolormode" || target == "basecolor") s->Output->BaseColorMode.reset();
            else return JS_ThrowTypeError(ctx, "unknown q.runtime.clear target");
            return JS_TRUE;
        }
'''
text = replace_once(text, anchor, anchor + clear_fn, "runtime clear API")
text = text.replace('JS_SetPropertyStr(ctx,graph,"setScriptEnabled",JS_NewCFunctionMagic(ctx,setEnabled,"setScriptEnabled",2,JS_CFUNC_generic_magic,2));', '')
text = replace_once(text, 'JS_SetPropertyStr(ctx,runtime,"previousShader",JS_NewCFunction(ctx,runtimePreviousShader,"previousShader",0));JS_SetPropertyStr(ctx,api,"runtime",runtime);', 'JS_SetPropertyStr(ctx,runtime,"previousShader",JS_NewCFunction(ctx,runtimePreviousShader,"previousShader",0));JS_SetPropertyStr(ctx,runtime,"clear",JS_NewCFunction(ctx,runtimeClear,"clear",1));JS_SetPropertyStr(ctx,api,"runtime",runtime);', "install runtime clear")
write(path, text)

# The application owns JavaScriptRuntime separately from RuntimeBindingEngine and evaluates it every main-loop iteration.
path = "src/Application.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/Model.hpp"\n', '#include "quartz/client/Model.hpp"\n#include "quartz/client/runtime/JavaScriptRuntime.hpp"\n#include "quartz/client/runtime/QuickJS.hpp"\n', "Application JS includes")
text = replace_once(text, '    RuntimeBindingEngine runtimeBindings;\n    RuntimeTelemetry runtimeTelemetry;', '    RuntimeBindingEngine runtimeBindings;\n    JavaScriptRuntime javascript(runtimeBindings);\n    RuntimeTelemetry runtimeTelemetry;', "Application JS instance")
text = replace_once(text, '    const double startTime = glfwGetTime();\n    keyboardInput.start(startTime);', '    const double startTime = glfwGetTime();\n    double lastJavaScriptFrame = startTime;\n    std::string javascriptObservedShaderId = settings.ShaderId;\n    std::string javascriptPreviousShaderId = settings.ShaderId;\n    keyboardInput.start(startTime);', "Application JS timing state")
text = replace_once(text, '        runtimeBindings.pollScriptReloadHotkey(window.handle(), keyboardInput);', '        javascript.pollReloadHotkey(window.handle(), keyboardInput);', "JavaScript reload hotkey")
visualizer_marker = '        const double visualizerFrameTime = 1.0 / std::max(1, settings.FrameRate);\n'
js_block = r'''        if (!settings.ShaderId.empty() && settings.ShaderId != javascriptObservedShaderId)
        {
            javascriptPreviousShaderId = javascriptObservedShaderId;
            javascriptObservedShaderId = settings.ShaderId;
        }
        PerformanceSnapshot javascriptPerformance{};
        bool javascriptHasPerformance = false;
        {
            std::lock_guard lock(deviceState.Mutex);
            javascriptPerformance = deviceState.Performance;
            javascriptHasPerformance = deviceState.HasPerformance;
        }
        RuntimeSignalContext javascriptContext;
        javascriptContext.Time = currentFrame;
        javascriptContext.DeltaTime = static_cast<float>(std::clamp(currentFrame - lastJavaScriptFrame, 0.0, 1.0));
        lastJavaScriptFrame = currentFrame;
        javascriptContext.Audio = audio.levelSnapshot();
        javascriptContext.MappedBands = &mappedBands;
        javascriptContext.SmoothedBands = &smoothedBands;
        javascriptContext.MediaColor = visualizerColor;
        javascriptContext.MediaAmount = mediaColorAmount;
        javascriptContext.MediaPlaying = mediaColor.playing();
        javascriptContext.MediaTitle = mediaColor.mediaTitle();
        javascriptContext.Keys = reactiveKeys;
        javascriptContext.Performance = javascriptPerformance;
        javascriptContext.HasPerformance = javascriptHasPerformance;
        javascriptContext.AppCpu = appCpuUsage;
        javascriptContext.USBConnected = usb.isConnected();
        javascriptContext.USB = usb.stats();
        javascriptContext.USBRates = runtimeBindings.usbRates();
        javascriptContext.Framebuffer = &framebuffer;
        javascriptContext.EffectiveGain = autoGain.EffectiveGain;
        javascriptContext.GainCorrection = autoGain.Correction;
        javascriptContext.CurrentShaderPreset = settings.ShaderPresetIndex;
        javascriptContext.CurrentShaderId = settings.ShaderId;
        javascriptContext.PreviousShaderId = javascriptPreviousShaderId;
        javascriptContext.ShaderTransitionActive = shaderTransition.Active;
        javascriptContext.ShaderTransitionProgress = shaderTransition.Active ? std::clamp(static_cast<float>((currentFrame - shaderTransition.StartedAt) / std::max(shaderTransition.Duration, 0.0001f)), 0.0f, 1.0f) : 1.0f;
        javascriptContext.BaseColorMode = settings.BaseColorMode;
        javascriptContext.GlobalBrightness = settings.GlobalBrightness;
        javascriptContext.SendFramebuffer = settings.SendFramebuffer;
        javascriptContext.ShaderFramebufferWidth = settings.ShaderFramebufferWidth;
        javascriptContext.ShaderFramebufferHeight = settings.ShaderFramebufferHeight;
        javascript.syncProfile(runtimeBindings);
        const RuntimeControlOutput& mainScriptOutput = runtimeEvaluateWorkspaceScripts(javascript, runtimeBindings, javascriptContext, shaderFramebuffer, keyboardInput);
        if (mainScriptOutput.ShaderId && *mainScriptOutput.ShaderId != settings.ShaderId)
        {
            switchShaderId(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, *mainScriptOutput.ShaderId, currentFrame, mainScriptOutput.ShaderTransitionSeconds, false);
        }
        else if (mainScriptOutput.ShaderPresetIndex && *mainScriptOutput.ShaderPresetIndex != settings.ShaderPresetIndex)
        {
            switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, *mainScriptOutput.ShaderPresetIndex, currentFrame, mainScriptOutput.ShaderTransitionSeconds, false);
        }

'''
text = replace_once(text, visualizer_marker, js_block + visualizer_marker, "independent JavaScript loop")
text = replace_once(text, '            const RuntimeControlOutput scriptOutput = runtimeEvaluateWorkspaceScripts(runtimeBindings, runtimeContext, shaderFramebuffer);', '            const RuntimeControlOutput& scriptOutput = javascript.output();', "legacy visualizer JS evaluation removal")
text = replace_once(text, 'deviceState, runtimeBindings, runtimeTelemetry, autoGain', 'deviceState, runtimeBindings, javascript, runtimeTelemetry, autoGain', "drawUi JavaScript argument")
text = replace_once(text, '            runtimeBindings.saveIfChanged();\n            nextSettingsSave', '            runtimeBindings.saveIfChanged();\n            javascript.saveIfChanged();\n            nextSettingsSave', "periodic JS save")
text = replace_once(text, '    runtimeBindings.save();\n    keyboardInput.stop();', '    runtimeBindings.save();\n    javascript.save();\n    keyboardInput.stop();', "shutdown JS save")
write(path, text)

print("JavaScript runtime integration patches applied")
