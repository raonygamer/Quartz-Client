#include "QuickJSInternal.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::client
{
    namespace
    {
        bool identifierStart(const char c) noexcept { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
        bool identifierContinue(const char c) noexcept { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

        std::vector<bool> codeMask(const std::string_view source)
        {
            std::vector<bool> code(source.size(), true);
            for (std::size_t i = 0; i < source.size();)
            {
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/')
                {
                    code[i++] = false; code[i++] = false;
                    while (i < source.size() && source[i] != '\n') code[i++] = false;
                    continue;
                }
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*')
                {
                    code[i++] = false; code[i++] = false;
                    while (i < source.size())
                    {
                        code[i] = false;
                        if (source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/') { code[i + 1] = false; i += 2; break; }
                        ++i;
                    }
                    continue;
                }
                if (source[i] == '\'' || source[i] == '"' || source[i] == '`')
                {
                    const char quote = source[i]; code[i++] = false; bool escaped = false;
                    while (i < source.size())
                    {
                        code[i] = false; const char c = source[i++];
                        if (escaped) { escaped = false; continue; }
                        if (c == '\\') { escaped = true; continue; }
                        if (c == quote) break;
                    }
                    continue;
                }
                ++i;
            }
            return code;
        }

        void blank(std::string& source, const std::size_t begin, const std::size_t end)
        {
            for (std::size_t i = begin; i < std::min(end, source.size()); ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
        }

        std::size_t skipSpace(const std::string& source, std::size_t i)
        {
            while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i]))) ++i;
            return i;
        }

        bool wordAt(const std::string& source, const std::vector<bool>& code, const std::size_t i, const std::string_view word)
        {
            if (i + word.size() > source.size()) return false;
            if (i && identifierContinue(source[i - 1])) return false;
            if (i + word.size() < source.size() && identifierContinue(source[i + word.size()])) return false;
            for (std::size_t j = 0; j < word.size(); ++j) if (!code[i + j] || source[i + j] != word[j]) return false;
            return true;
        }

        std::size_t matching(const std::string& source, const std::vector<bool>& code, const std::size_t open, const char left, const char right)
        {
            int depth = 0;
            for (std::size_t i = open; i < source.size(); ++i)
            {
                if (!code[i]) continue;
                if (source[i] == left) ++depth;
                else if (source[i] == right && --depth == 0) return i;
            }
            return std::string::npos;
        }

        std::size_t typeEnd(const std::string& source, const std::vector<bool>& code, const std::size_t start, const std::size_t limit, const bool parameter)
        {
            int paren = 0, brace = 0, bracket = 0, angle = 0;
            for (std::size_t i = start; i < limit; ++i)
            {
                if (!code[i]) continue;
                const char c = source[i];
                if (c == '(') ++paren; else if (c == ')' && paren > 0) --paren;
                else if (c == '{') ++brace; else if (c == '}' && brace > 0) --brace;
                else if (c == '[') ++bracket; else if (c == ']' && bracket > 0) --bracket;
                else if (c == '<') ++angle; else if (c == '>' && angle > 0) --angle;
                if (paren || brace || bracket || angle) continue;
                if (parameter && c == ',') return i;
                if (!parameter && (c == '=' || c == ',' || c == ';')) return i;
            }
            return limit;
        }

        void stripImportTypes(std::string& output, const std::vector<bool>& code)
        {
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                if (!wordAt(output, code, i, "import")) continue;
                const std::size_t type = skipSpace(output, i + 6);
                if (!wordAt(output, code, type, "type")) continue;
                std::size_t end = type + 4;
                while (end < output.size() && (!code[end] || output[end] != ';')) ++end;
                blank(output, i, std::min(end + 1, output.size()));
            }
        }

        void stripDeclarations(std::string& output, const std::vector<bool>& code)
        {
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                if (wordAt(output, code, i, "interface"))
                {
                    std::size_t open = i + 9; while (open < output.size() && (!code[open] || output[open] != '{')) ++open;
                    if (open == output.size()) continue;
                    const std::size_t close = matching(output, code, open, '{', '}'); if (close == std::string::npos) continue;
                    std::size_t end = close + 1; while (end < output.size() && std::isspace(static_cast<unsigned char>(output[end])) && output[end] != '\n') ++end; if (end < output.size() && output[end] == ';') ++end;
                    blank(output, i, end); i = end ? end - 1 : end; continue;
                }
                if (!wordAt(output, code, i, "type")) continue;
                std::size_t previous = i; while (previous && std::isspace(static_cast<unsigned char>(output[previous - 1]))) --previous;
                if (previous && output[previous - 1] != ';' && output[previous - 1] != '{' && output[previous - 1] != '}') continue;
                int brace = 0, bracket = 0, paren = 0, angle = 0; std::size_t end = i + 4;
                for (; end < output.size(); ++end)
                {
                    if (!code[end]) continue; const char c = output[end];
                    if (c == '{') ++brace; else if (c == '}' && brace > 0) --brace; else if (c == '[') ++bracket; else if (c == ']' && bracket > 0) --bracket; else if (c == '(') ++paren; else if (c == ')' && paren > 0) --paren; else if (c == '<') ++angle; else if (c == '>' && angle > 0) --angle;
                    if (!brace && !bracket && !paren && !angle && c == ';') { ++end; break; }
                }
                blank(output, i, end); i = end ? end - 1 : end;
            }
        }

        std::size_t declarationDelimiter(const std::string& source, const std::vector<bool>& code, const std::size_t start)
        {
            int paren = 0, brace = 0, bracket = 0;
            for (std::size_t i = start; i < source.size(); ++i)
            {
                if (!code[i]) continue; const char c = source[i];
                if (c == '(') ++paren; else if (c == ')' && paren > 0) --paren;
                else if (c == '{') ++brace; else if (c == '}' && brace > 0) --brace;
                else if (c == '[') ++bracket; else if (c == ']' && bracket > 0) --bracket;
                if (!paren && !brace && !bracket && (c == ',' || c == ';')) return i;
            }
            return source.size();
        }

        void stripVariableTypes(std::string& output, const std::vector<bool>& code)
        {
            static constexpr std::string_view Keywords[] = {"let", "const", "var"};
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                std::size_t keywordLength = 0; for (const auto keyword : Keywords) if (wordAt(output, code, i, keyword)) { keywordLength = keyword.size(); break; }
                if (!keywordLength) continue;
                std::size_t cursor = skipSpace(output, i + keywordLength);
                while (cursor < output.size())
                {
                    if (!identifierStart(output[cursor])) break;
                    ++cursor; while (cursor < output.size() && identifierContinue(output[cursor])) ++cursor; cursor = skipSpace(output, cursor);
                    if (cursor < output.size() && code[cursor] && output[cursor] == ':')
                    {
                        const std::size_t end = typeEnd(output, code, cursor + 1, output.size(), false); blank(output, cursor, end); cursor = skipSpace(output, end);
                    }
                    if (cursor < output.size() && code[cursor] && output[cursor] == '=') ++cursor;
                    const std::size_t delimiter = declarationDelimiter(output, code, cursor);
                    if (delimiter >= output.size() || output[delimiter] == ';') break;
                    cursor = skipSpace(output, delimiter + 1);
                }
            }
        }

        void stripFunctionTypes(std::string& output, const std::vector<bool>& code)
        {
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                if (!wordAt(output, code, i, "function")) continue;
                std::size_t open = i + 8; while (open < output.size() && (!code[open] || output[open] != '(')) ++open;
                if (open == output.size()) continue;
                const std::size_t close = matching(output, code, open, '(', ')'); if (close == std::string::npos) continue;
                int paren = 0, brace = 0, bracket = 0;
                for (std::size_t cursor = open + 1; cursor < close; ++cursor)
                {
                    if (!code[cursor]) continue; const char c = output[cursor];
                    if (c == '(') ++paren; else if (c == ')' && paren > 0) --paren; else if (c == '{') ++brace; else if (c == '}' && brace > 0) --brace; else if (c == '[') ++bracket; else if (c == ']' && bracket > 0) --bracket;
                    if (c != ':' || paren || brace || bracket) continue;
                    const std::size_t end = typeEnd(output, code, cursor + 1, close, true); blank(output, cursor, end); cursor = end ? end - 1 : end;
                }
                std::size_t after = skipSpace(output, close + 1);
                if (after < output.size() && code[after] && output[after] == ':')
                {
                    std::size_t end = after + 1; int parenDepth = 0, braceDepth = 0, bracketDepth = 0, angleDepth = 0;
                    for (; end < output.size(); ++end)
                    {
                        if (!code[end]) continue; const char c = output[end];
                        if (c == '(') ++parenDepth; else if (c == ')' && parenDepth > 0) --parenDepth; else if (c == '[') ++bracketDepth; else if (c == ']' && bracketDepth > 0) --bracketDepth; else if (c == '<') ++angleDepth; else if (c == '>' && angleDepth > 0) --angleDepth;
                        if (!parenDepth && !braceDepth && !bracketDepth && !angleDepth && c == '{') break;
                    }
                    blank(output, after, end);
                }
                i = close;
            }
        }
    }

    bool runtimeTranspileTypeScript(const std::string_view source, std::string& output, std::string& error) noexcept
    {
        try
        {
            output.assign(source); const auto code = codeMask(source);
            stripImportTypes(output, code); stripDeclarations(output, code); stripVariableTypes(output, code); stripFunctionTypes(output, code);
            for (std::size_t i = 0; i < output.size(); ++i)
                if (wordAt(output, code, i, "enum") || wordAt(output, code, i, "namespace")) { error = "TypeScript enum/namespace syntax is not supported by Quartz's embedded type eraser yet"; return false; }
            error.clear(); return true;
        }
        catch (const std::exception& exception) { error = exception.what(); return false; }
        catch (...) { error = "unknown TypeScript transpilation error"; return false; }
    }

    std::string_view runtimeQuickJSSDKModuleSource() noexcept
    {
        static constexpr std::string_view Source = R"JS(import { api } from "@quartz/native";

function subscription(id) {
    let active = true;
    return Object.freeze({
        get active() { return active; },
        dispose() { if (!active) return; active = false; api.events.unsubscribe(id); }
    });
}

function listen(type, callback) { return subscription(api.events.subscribe(type, callback)); }
function address(value) { return typeof value === "bigint" ? value : BigInt(value); }

const processCache = new Map();
function processFrom(info) {
    if (!info) return undefined;
    let process = processCache.get(info.pid);
    if (process) { process.__info = info; return process; }
    process = {
        __info: info,
        get pid() { return this.__info.pid; },
        get name() { return this.__info.name; },
        get exe() { return this.__info.exe; },
        get title() { return this.__info.title; },
        get commandLine() { return this.__info.commandLine; },
        get alive() { return api.process.alive(this.pid); },
        modules() { return api.process.modules(this.pid); },
        regions() { return api.process.regions(this.pid); },
        on(event, callback) {
            if (event !== "stopped") throw new TypeError(`unknown Process event: ${event}`);
            return listen("process.stopped", value => { if (value.pid === this.pid) callback({ process: this, time: value.time }); });
        }
    };
    processCache.set(info.pid, process);
    return process;
}

export const Process = Object.freeze({
    list() { return api.process.list().map(processFrom); },
    find(query) { return processFrom(api.process.find(query)); },
    wait(query) {
        const existing = processFrom(api.process.find(query));
        if (existing) return Promise.resolve(existing);
        return new Promise(resolve => {
            const sub = listen("process.started", () => {
                const found = processFrom(api.process.find(query));
                if (!found) return;
                sub.dispose(); resolve(found);
            });
        });
    }
});

function signatureStatus(id) { return api.signature.status(id); }
export const Signature = Object.freeze({
    scan(process, pattern, options = {}) {
        const id = api.signature.scan(process.pid, pattern, options.executableOnly !== false);
        let resolvePromise, rejectPromise, lastStatus = signatureStatus(id);
        const promise = new Promise((resolve, reject) => { resolvePromise = resolve; rejectPromise = reject; });
        const completion = listen("signature.finished", event => {
            if (event.id !== id) return;
            lastStatus = signatureStatus(id) || lastStatus; completion.dispose();
            if (event.type === "signature.found") resolvePromise(event.address);
            else if (event.type === "signature.cancelled") rejectPromise(new Error("signature scan cancelled"));
            else if (event.type === "signature.error") rejectPromise(new Error(event.error || "signature scan failed"));
            else rejectPromise(new Error("signature not found"));
        });
        Object.defineProperties(promise, {
            id: { value: id }, process: { value: process }, pattern: { value: pattern },
            progress: { get() { lastStatus = signatureStatus(id) || lastStatus; return lastStatus?.progress ?? 0; } },
            scannedBytes: { get() { lastStatus = signatureStatus(id) || lastStatus; return lastStatus?.scannedBytes ?? 0n; } },
            totalBytes: { get() { lastStatus = signatureStatus(id) || lastStatus; return lastStatus?.totalBytes ?? 0n; } },
            averageMiBs: { get() { lastStatus = signatureStatus(id) || lastStatus; return lastStatus?.averageMiBs ?? 0; } },
            finished: { get() { lastStatus = signatureStatus(id) || lastStatus; return lastStatus?.finished ?? false; } }
        });
        promise.cancel = () => { api.signature.cancel(id); };
        promise.on = (event, callback) => {
            if (event === "progress") {
                let previous = -1;
                return listen("__quartz.frame", () => {
                    const status = signatureStatus(id); if (!status || status.finished || status.progress === previous) return;
                    previous = status.progress; callback({ scan: promise, progress: status.progress, scannedBytes: status.scannedBytes, totalBytes: status.totalBytes, averageMiBs: status.averageMiBs });
                });
            }
            if (event === "cancelled") return listen("signature.cancelled", value => { if (value.id === id) callback(); });
            throw new TypeError(`unknown SignatureScan event: ${event}`);
        };
        return promise;
    }
});

function breakpointAt(process, targetAddress) {
    const target = address(targetAddress);
    const result = {
        process,
        address: target,
        get armed() { return api.breakpoint.running(); },
        arm() { api.breakpoint.arm(process.pid, target); },
        cancel() { api.breakpoint.cancel(); },
        on(event, callback) {
            if (event !== "hit") throw new TypeError(`unknown Breakpoint event: ${event}`);
            return listen("breakpoint.hit", value => { if (value.pid === process.pid && value.address === target) callback({ ...value, process }); });
        },
        nextHit(options = {}) {
            return new Promise((resolve, reject) => {
                const started = api.time;
                const hitSub = this.on("hit", value => { hitSub.dispose(); frameSub.dispose(); resolve(value); });
                const frameSub = listen("__quartz.frame", event => {
                    if (!(options.timeout > 0) || event.time - started < options.timeout) return;
                    hitSub.dispose(); frameSub.dispose(); api.breakpoint.cancel(); reject(new Error("breakpoint timeout"));
                });
                try { this.arm(); } catch (error) { hitSub.dispose(); frameSub.dispose(); reject(error); }
            });
        }
    };
    return result;
}

export const Breakpoint = Object.freeze({ at: breakpointAt });

const sizes = Object.freeze({ i8: 1n, u8: 1n, i16: 2n, u16: 2n, i32: 4n, u32: 4n, i64: 8n, u64: 8n, f32: 4n, f64: 8n, bool: 1n });
function scalar(kind, offset) { return Object.freeze({ kind, offset: address(offset), size: sizes[kind] }); }
function fieldAddress(base, field) { return base + field.offset; }
function readField(process, base, field) {
    const current = fieldAddress(base, field);
    if (field.kind === "pointer") {
        const value = api.memory.read(process.pid, current, "ptr");
        if (!value) return undefined;
        return field.struct ? field.struct.at(process, value) : value;
    }
    if (field.kind === "struct") return field.struct.at(process, current);
    if (field.kind === "array") {
        if (!field.element.size) throw new TypeError("array element has no fixed size");
        const values = [];
        for (let i = 0; i < field.count; ++i) values.push(readField(process, current + BigInt(i) * field.element.size, { ...field.element, offset: field.element.offset || 0n }));
        return values;
    }
    return api.memory.read(process.pid, current, field.kind);
}
function writeField(process, base, field, value) {
    const current = fieldAddress(base, field);
    if (field.kind === "struct" || field.kind === "array") throw new TypeError("embedded structs/arrays are not directly assignable");
    if (field.kind === "pointer") {
        const pointer = value && typeof value === "object" && "$address" in value ? value.$address : value;
        api.memory.write(process.pid, current, "ptr", pointer || 0n); return;
    }
    api.memory.write(process.pid, current, field.kind, value);
}
function pointerFor(type, process, targetAddress) {
    const target = {
        $process: process,
        $address: address(targetAddress),
        get $readable() { return this.isReadable(); },
        isReadable() { try { api.memory.read(process.pid, this.$address, "u8"); return true; } catch { return false; } },
        as(nextType) { return pointerFor(nextType, process, this.$address); }
    };
    return new Proxy(target, {
        get(object, key, receiver) {
            if (Reflect.has(object, key)) return Reflect.get(object, key, receiver);
            const field = type.fields[key]; return field ? readField(process, object.$address, field) : undefined;
        },
        set(object, key, value, receiver) {
            const field = type.fields[key]; if (!field) return Reflect.set(object, key, value, receiver);
            writeField(process, object.$address, field, value); return true;
        }
    });
}

export const Field = Object.freeze({
    Int8: offset => scalar("i8", offset), UInt8: offset => scalar("u8", offset),
    Int16: offset => scalar("i16", offset), UInt16: offset => scalar("u16", offset),
    Int32: offset => scalar("i32", offset), UInt32: offset => scalar("u32", offset),
    Int64: offset => scalar("i64", offset), UInt64: offset => scalar("u64", offset),
    Float32: offset => scalar("f32", offset), Float64: offset => scalar("f64", offset), Boolean: offset => scalar("bool", offset),
    Pointer(offset, struct) { return Object.freeze({ kind: "pointer", offset: address(offset), struct }); },
    Struct(offset, struct) { return Object.freeze({ kind: "struct", offset: address(offset), struct }); },
    Array(offset, element, count) { return Object.freeze({ kind: "array", offset: address(offset), element, count }); }
});

export const Struct = Object.freeze({
    define(fields) {
        const type = { fields: Object.freeze({ ...fields }), at(process, targetAddress) { return pointerFor(type, process, targetAddress); } };
        return Object.freeze(type);
    }
});

function property(kind, id, options) {
    api.sdk.propertyRegister(kind, id, options);
    return Object.freeze({
        get value() { return api.sdk.propertyGet(id); },
        set value(value) { api.sdk.propertySet(id, value); },
        get defaultValue() { return options.default; },
        reset() { api.sdk.propertyReset(id); },
        on(event, callback) {
            if (event !== "changed") throw new TypeError(`unknown Property event: ${event}`);
            return listen("property.changed", value => { if (value.id === id) callback(value.value); });
        }
    });
}

export const Property = Object.freeze({
    String: (id, options) => property("string", id, options), Boolean: (id, options) => property("boolean", id, options),
    Int32: (id, options) => property("int32", id, options), UInt32: (id, options) => property("uint32", id, options),
    Float32: (id, options) => property("float32", id, options), Float64: (id, options) => property("float64", id, options),
    Shader: (id, options) => property("shader", id, options), File: (id, options) => property("file", id, options), Directory: (id, options) => property("directory", id, options),
    Key: (id, options) => property("key", id, options), Enum: (id, options) => property("enum", id, options)
});

export const Script = Object.freeze({
    configure(configuration) { api.sdk.scriptConfigure(configuration); },
    reload() { api.sdk.scriptReload(); },
    get id() { return api.sdk.scriptId(); },
    get name() { return api.name; },
    get state() { return api.state; },
    get storage() { return api.storage; }
});

export const System = Object.freeze({
    get time() { return api.time; },
    on(event, callback) {
        if (event === "initialize") return listen("script.initialize", () => callback());
        if (event === "update") return listen("tick", value => callback({ time: value.time, deltaTime: value.deltaTime }));
        if (event === "dispose") return listen("script.dispose", value => callback({ reason: value.reason }));
        throw new TypeError(`unknown System event: ${event}`);
    },
    sleep(seconds) {
        const target = api.time + Math.max(0, Number(seconds));
        return new Promise(resolve => {
            const sub = listen("__quartz.frame", event => { if (event.time < target) return; sub.dispose(); resolve(); });
        });
    }
});

export const Runtime = Object.freeze({
    currentShader: () => api.runtime.currentShader(), previousShader: () => api.runtime.previousShader(),
    setShader: (id, transitionSeconds) => api.runtime.shader(id, transitionSeconds),
    setShaderPreset: (index, transitionSeconds) => api.runtime.shaderPreset(index, transitionSeconds),
    setMaterial: (id, component, value) => api.runtime.material(id, component, value),
    setBrightness: value => api.runtime.brightness(value), setFramebufferEnabled: enabled => api.runtime.sendFramebuffer(enabled), setBaseColorMode: mode => api.runtime.baseColorMode(mode),
    clearShader: () => api.runtime.clear("shader"), clear: () => api.runtime.clear("all")
});

export const Disassembly = Object.freeze({ decode: (process, targetAddress, count) => api.disassembly.decode(process.pid, targetAddress, count) });
export const Memory = Object.freeze({
    readBytes(process, targetAddress, length) { return Uint8Array.from(api.memory.readBytes(process.pid, targetAddress, length)); },
    writeBytes(process, targetAddress, bytes) { api.memory.writeBytes(process.pid, targetAddress, typeof bytes === "string" ? bytes : Array.from(bytes)); }
});
export const Keyboard = Object.freeze({
    keyDown: key => api.input.keyDown(key), shortcut: (key, modifiers) => api.input.shortcut(key, modifiers),
    get capsLock() { return api.input.capsLock(); }, get scrollLock() { return api.input.scrollLock(); }
});
export const Events = Object.freeze({ on: (type, callback) => listen(type, callback), emit: (type, data) => { api.events.emit(type, data); } });
)JS";
        return Source;
    }
}
