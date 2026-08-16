#include "QuickJSInternal.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/native/ExecutionProbe.hpp"
#include "quartz/client/shader/ShaderFramebuffer.hpp"
#include <quickjs.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace quartz::client
{
    namespace
    {
        constexpr std::size_t WorkspaceMemoryLimit = 64ULL * 1024ULL * 1024ULL;
        constexpr std::size_t WorkspaceStackLimit = 512ULL * 1024ULL;

        std::uint64_t hashText(const std::string_view text) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
            return hash;
        }

        std::string exceptionText(JSContext* ctx)
        {
            JSValue exception = JS_GetException(ctx);
            std::string result = "QuickJS exception";
            if (const char* text = JS_ToCString(ctx, exception)) { result = text; JS_FreeCString(ctx, text); }
            if (JS_IsObject(exception))
            {
                JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
                if (!JS_IsUndefined(stack))
                    if (const char* text = JS_ToCString(ctx, stack))
                    {
                        if (*text && result.find(text) == std::string::npos) { result += '\n'; result += text; }
                        JS_FreeCString(ctx, text);
                    }
                JS_FreeValue(ctx, stack);
            }
            JS_FreeValue(ctx, exception);
            return result;
        }

        struct Subscription
        {
            std::uint64_t Id = 0;
            std::string Type;
            JSValue Callback = JS_UNDEFINED;
        };

        struct Instance : RuntimeQuickJSContext
        {
            JSContext* Context = nullptr;
            JSValue Function = JS_UNDEFINED;
            JSValue Api = JS_UNDEFINED;
            JSValue SnapshotFunction = JS_UNDEFINED;
            std::uint64_t SourceHash = 0;
            std::filesystem::path MainPath;
            std::filesystem::path CurrentDirectory;
            std::filesystem::file_time_type MainTime{};
            std::unordered_map<std::string, std::filesystem::file_time_type> DependencyTimes;
            std::unordered_map<std::string, JSValue> Modules;
            std::vector<Subscription> Subscriptions;
            std::uint64_t NextSubscriptionId = 1;
            std::array<float, MatrixSize> PreviousKeys{};
            bool KeysInitialized = false;
            bool PreviousCaps = false;
            bool PreviousScroll = false;
            std::string PreviousShader;
            std::unordered_map<pid_t, std::string> Processes;
            bool ProcessesInitialized = false;
            double NextProcessPoll = 0.0;
            double LastBreakpointHitTime = -1.0;
            bool FirstExecution = true;
            bool Reloaded = false;

            ~Instance()
            {
                if (Script) runtimeCancelQuickJSSignatureScans(*Script);
                if (!Context) return;
                for (auto& [_, value] : Modules) JS_FreeValue(Context, value);
                for (auto& subscription : Subscriptions) JS_FreeValue(Context, subscription.Callback);
                JS_FreeValue(Context, SnapshotFunction);
                JS_FreeValue(Context, Function);
                JS_FreeValue(Context, Api);
                JS_FreeContext(Context);
            }
        };

        struct Workspace
        {
            JSRuntime* Runtime = nullptr;
            RuntimeQuickJSDeadline Execution{};
            std::unordered_map<std::uint64_t, std::unique_ptr<Instance>> Instances;

            Workspace()
            {
                Runtime = JS_NewRuntime();
                if (!Runtime) return;
                JS_SetRuntimeInfo(Runtime, "Quartz JavaScript runtime");
                JS_SetMemoryLimit(Runtime, WorkspaceMemoryLimit);
                JS_SetMaxStackSize(Runtime, WorkspaceStackLimit);
                JS_SetCanBlock(Runtime, false);
                JS_SetInterruptHandler(Runtime, [](JSRuntime*, void* opaque)
                {
                    auto* self = static_cast<Workspace*>(opaque);
                    if (!self || !self->Execution.Active || std::chrono::steady_clock::now() < self->Execution.Deadline) return 0;
                    self->Execution.Interrupted = true;
                    return 1;
                }, this);
            }

            ~Workspace()
            {
                Instances.clear();
                if (Runtime) JS_FreeRuntime(Runtime);
            }

            void begin(const float milliseconds)
            {
                Execution.Interrupted = false;
                Execution.Active = true;
                Execution.Deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(std::clamp(milliseconds, 0.1f, 100.0f)));
            }

            void end() noexcept { Execution.Active = false; }
        };

        Workspace& workspace()
        {
            static Workspace value;
            return value;
        }

        void appendLog(RuntimeScript& script, const double time, const RuntimeScriptLogLevel level, std::string text)
        {
            script.LastLog = text;
            script.Console.push_back({time, level, std::move(text)});
            constexpr std::size_t MaximumConsoleEntries = 512;
            if (script.Console.size() > MaximumConsoleEntries) script.Console.erase(script.Console.begin(), script.Console.begin() + static_cast<std::ptrdiff_t>(script.Console.size() - MaximumConsoleEntries));
        }

        std::string logText(JSContext* ctx, const int argc, JSValueConst* argv)
        {
            std::string text;
            for (int i = 0; i < argc; ++i)
            {
                const char* value = JS_ToCString(ctx, argv[i]); if (!value) return {};
                if (!text.empty()) text.push_back(' ');
                if (text.size() < 4096) text.append(value, std::min<std::size_t>(std::strlen(value), 4096 - text.size()));
                JS_FreeCString(ctx, value);
            }
            return text;
        }

        JSValue jsLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            auto* instance = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); if (!instance || !instance->Script) return JS_UNDEFINED;
            appendLog(*instance->Script, instance->SignalContext ? instance->SignalContext->Time : 0.0, RuntimeScriptLogLevel::Info, logText(ctx, argc, argv)); return JS_UNDEFINED;
        }

        JSValue jsConsoleLog(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv, const int magic)
        {
            auto* instance = static_cast<RuntimeQuickJSContext*>(JS_GetContextOpaque(ctx)); if (!instance || !instance->Script) return JS_UNDEFINED;
            const auto level = magic == 0 ? RuntimeScriptLogLevel::Debug : magic == 2 ? RuntimeScriptLogLevel::Warning : magic == 3 ? RuntimeScriptLogLevel::Error : RuntimeScriptLogLevel::Info;
            appendLog(*instance->Script, instance->SignalContext ? instance->SignalContext->Time : 0.0, level, logText(ctx, argc, argv)); return JS_UNDEFINED;
        }

        bool loadFile(const std::filesystem::path& path, std::string& text, std::string& error) noexcept
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
            {
                error = ec ? "could not inspect " + path.string() + ": " + ec.message() : std::filesystem::is_directory(path, ec) ? "script path is a directory: " + path.string() : "script path is not a regular file: " + path.string();
                return false;
            }
            try
            {
                std::ifstream file(path, std::ios::binary); if (!file) { error = "could not open " + path.string(); return false; }
                text.assign(std::istreambuf_iterator<char>(file), {});
                if (!file && !file.eof()) { error = "could not read " + path.string(); return false; }
                error.clear(); return true;
            }
            catch (const std::exception& exception) { error = "could not read " + path.string() + ": " + exception.what(); return false; }
            catch (...) { error = "could not read " + path.string() + ": unknown filesystem error"; return false; }
        }

        std::filesystem::file_time_type fileTime(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            return ec ? std::filesystem::file_time_type::min() : time;
        }

        JSValue makeEvent(JSContext* ctx, const char* type, const double time)
        {
            JSValue event = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, type));
            JS_SetPropertyStr(ctx, event, "time", JS_NewFloat64(ctx, time));
            return event;
        }

        bool wantsEvent(const Instance& instance, const std::string_view type)
        {
            return std::ranges::any_of(instance.Subscriptions, [&](const Subscription& subscription) { return subscription.Type == type || subscription.Type == "*"; });
        }

        bool dispatchEvent(Instance& instance, const std::string_view type, JSValueConst event, std::string& error)
        {
            std::vector<JSValue> callbacks;
            for (const auto& subscription : instance.Subscriptions)
                if (subscription.Type == type || subscription.Type == "*") callbacks.emplace_back(JS_DupValue(instance.Context, subscription.Callback));
            for (std::size_t i = 0; i < callbacks.size(); ++i)
            {
                JSValue argument = JS_DupValue(instance.Context, event);
                JSValue result = JS_Call(instance.Context, callbacks[i], JS_UNDEFINED, 1, &argument);
                JS_FreeValue(instance.Context, argument);
                JS_FreeValue(instance.Context, callbacks[i]);
                callbacks[i] = JS_UNDEFINED;
                if (JS_IsException(result))
                {
                    error = exceptionText(instance.Context);
                    JS_FreeValue(instance.Context, result);
                    for (std::size_t j = i + 1; j < callbacks.size(); ++j) JS_FreeValue(instance.Context, callbacks[j]);
                    return false;
                }
                JS_FreeValue(instance.Context, result);
            }
            return true;
        }

        JSValue jsEventSubscribe(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "q.events.subscribe(type, callback): callback must be a function");
            const char* raw = JS_ToCString(ctx, argv[0]);
            if (!raw) return JS_EXCEPTION;
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx));
            const std::uint64_t id = instance->NextSubscriptionId++;
            instance->Subscriptions.push_back({id, raw, JS_DupValue(ctx, argv[1])});
            JS_FreeCString(ctx, raw);
            return JS_NewBigUint64(ctx, id);
        }

        JSValue jsEventUnsubscribe(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            if (argc < 1) return JS_FALSE;
            std::int64_t raw = 0;
            if (JS_ToInt64Ext(ctx, &raw, argv[0]) < 0 || raw <= 0) return JS_FALSE;
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx));
            const auto it = std::ranges::find(instance->Subscriptions, static_cast<std::uint64_t>(raw), &Subscription::Id);
            if (it == instance->Subscriptions.end()) return JS_FALSE;
            JS_FreeValue(ctx, it->Callback);
            instance->Subscriptions.erase(it);
            return JS_TRUE;
        }

        JSValue jsEventEmit(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            if (argc < 1) return JS_ThrowTypeError(ctx, "q.events.emit(type, data?): type required");
            const char* raw = JS_ToCString(ctx, argv[0]);
            if (!raw) return JS_EXCEPTION;
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx));
            const double time = instance->SignalContext ? instance->SignalContext->Time : 0.0;
            JSValue event = makeEvent(ctx, raw, time);
            if (argc > 1) JS_SetPropertyStr(ctx, event, "data", JS_DupValue(ctx, argv[1]));
            std::string error;
            const bool ok = dispatchEvent(*instance, raw, event, error);
            JS_FreeValue(ctx, event);
            JS_FreeCString(ctx, raw);
            return ok ? JS_TRUE : JS_ThrowInternalError(ctx, "%s", error.c_str());
        }

        JSValue jsImport(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)
        {
            if (argc < 1) return JS_ThrowTypeError(ctx, "q.import(path): path required");
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx));
            const char* raw = JS_ToCString(ctx, argv[0]);
            if (!raw) return JS_EXCEPTION;
            std::filesystem::path requested(raw);
            JS_FreeCString(ctx, raw);
            std::filesystem::path path = requested.is_absolute() ? requested : instance->CurrentDirectory / requested;
            std::error_code ec;
            path = std::filesystem::weakly_canonical(path, ec);
            if (ec) path = std::filesystem::absolute(path, ec);
            if (path.extension() != ".js" && path.extension() != ".mjs") return JS_ThrowTypeError(ctx, "q.import only accepts .js/.mjs files");
            const std::string key = path.string();
            if (const auto it = instance->Modules.find(key); it != instance->Modules.end()) return JS_DupValue(ctx, it->second);
            std::string source, readError;
            if (!loadFile(path, source, readError)) return JS_ThrowReferenceError(ctx, "%s", readError.c_str());
            instance->DependencyTimes[key] = fileTime(path);
            std::string wrapped = "(function(q,exports,module){\n\"use strict\";\n" + source + "\n;return module.exports;\n})";
            JSValue function = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), key.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
            if (JS_IsException(function)) return function;
            JSValue exports = JS_NewObject(ctx);
            JSValue module = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, module, "exports", JS_DupValue(ctx, exports));
            JSValue args[3]{JS_DupValue(ctx, instance->Api), JS_DupValue(ctx, exports), JS_DupValue(ctx, module)};
            const auto previousDirectory = instance->CurrentDirectory;
            instance->CurrentDirectory = path.parent_path();
            JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
            instance->CurrentDirectory = previousDirectory;
            for (auto& argument : args) JS_FreeValue(ctx, argument);
            JS_FreeValue(ctx, function);
            JS_FreeValue(ctx, exports);
            JS_FreeValue(ctx, module);
            if (JS_IsException(result)) return result;
            instance->Modules.emplace(key, JS_DupValue(ctx, result));
            return result;
        }

        Instance* createInstance(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)
        {
            auto& runtime = workspace();
            if (!runtime.Runtime) return nullptr;
            auto [it, inserted] = runtime.Instances.try_emplace(script.Id);
            if (!inserted) return it->second.get();
            auto instance = std::make_unique<Instance>();
            instance->Execution = &runtime.Execution;
            instance->JavaScript = &javascript;
            instance->Script = &script;
            instance->SignalContext = &signal;
            instance->Keyboard = &keyboard;
            instance->Shader = &shader;
            instance->Output = &output;
            instance->Reloaded = script.ReloadCount > 0;
            if (const auto oldHit = executionProbe().hit()) instance->LastBreakpointHitTime = oldHit->Time;
            instance->Context = JS_NewContext(runtime.Runtime);
            if (!instance->Context) { runtime.Instances.erase(it); return nullptr; }
            JS_SetContextOpaque(instance->Context, static_cast<RuntimeQuickJSContext*>(instance.get()));
            instance->Api = JS_NewObject(instance->Context);
            JS_SetPropertyStr(instance->Context, instance->Api, "log", JS_NewCFunction(instance->Context, jsLog, "log", 1));
            JSValue console = JS_NewObject(instance->Context);
            JS_SetPropertyStr(instance->Context, console, "debug", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "debug", 1, JS_CFUNC_generic_magic, 0));
            JS_SetPropertyStr(instance->Context, console, "log", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "log", 1, JS_CFUNC_generic_magic, 1));
            JS_SetPropertyStr(instance->Context, console, "info", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "info", 1, JS_CFUNC_generic_magic, 1));
            JS_SetPropertyStr(instance->Context, console, "warn", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "warn", 1, JS_CFUNC_generic_magic, 2));
            JS_SetPropertyStr(instance->Context, console, "error", JS_NewCFunctionMagic(instance->Context, jsConsoleLog, "error", 1, JS_CFUNC_generic_magic, 3));
            JS_SetPropertyStr(instance->Context, instance->Api, "console", console);
            JS_SetPropertyStr(instance->Context, instance->Api, "import", JS_NewCFunction(instance->Context, jsImport, "import", 1));
            runtimeInstallQuickJSLowLevelApi(instance->Context, instance->Api);
            runtimeInstallQuickJSAsyncSignatureApi(instance->Context, instance->Api);
            JSValue events = JS_NewObject(instance->Context);
            JS_SetPropertyStr(instance->Context, events, "subscribe", JS_NewCFunction(instance->Context, jsEventSubscribe, "subscribe", 2));
            JS_SetPropertyStr(instance->Context, events, "unsubscribe", JS_NewCFunction(instance->Context, jsEventUnsubscribe, "unsubscribe", 1));
            JS_SetPropertyStr(instance->Context, events, "emit", JS_NewCFunction(instance->Context, jsEventEmit, "emit", 2));
            JS_SetPropertyStr(instance->Context, instance->Api, "events", events);
            JS_DefinePropertyValueStr(instance->Context, instance->Api, "state", JS_NewObject(instance->Context), JS_PROP_ENUMERABLE);
            JSValue storage = JS_ParseJSON(instance->Context, script.PersistentStateJson.c_str(), script.PersistentStateJson.size(), "q.storage");
            if (JS_IsException(storage) || !JS_IsObject(storage))
            {
                if (JS_IsException(storage)) { JSValue exception = JS_GetException(instance->Context); JS_FreeValue(instance->Context, exception); }
                else JS_FreeValue(instance->Context, storage);
                storage = JS_NewObject(instance->Context);
            }
            JS_DefinePropertyValueStr(instance->Context, instance->Api, "storage", storage, JS_PROP_ENUMERABLE);
            static constexpr std::string_view SnapshotSource = "(value=>{const seen=new WeakSet();return JSON.stringify(value,(key,item)=>{if(typeof item==='bigint')return '0x'+item.toString(16)+'n';if(typeof item==='function')return '[Function]';if(typeof item==='object'&&item!==null){if(seen.has(item))return '[Circular]';seen.add(item);}return item;},2);})";
            instance->SnapshotFunction = JS_Eval(instance->Context, SnapshotSource.data(), SnapshotSource.size(), "quartz-state-snapshot.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
            if (JS_IsException(instance->SnapshotFunction)) { JSValue exception = JS_GetException(instance->Context); JS_FreeValue(instance->Context, exception); JS_FreeValue(instance->Context, instance->SnapshotFunction); instance->SnapshotFunction = JS_UNDEFINED; }
            auto* result = instance.get();
            it->second = std::move(instance);
            return result;
        }

        bool dependenciesChanged(const Instance& instance)
        {
            for (const auto& [path, time] : instance.DependencyTimes) if (fileTime(path) != time) return true;
            return false;
        }

        bool compile(Instance& instance, RuntimeScript& script, const std::string& source, const std::filesystem::path& filename, std::string& error)
        {
            const auto hash = hashText(source);
            if (instance.SourceHash == hash && JS_IsFunction(instance.Context, instance.Function)) return true;
            JS_FreeValue(instance.Context, instance.Function);
            instance.Function = JS_UNDEFINED;
            instance.SourceHash = hash;
            for (auto& [_, value] : instance.Modules) JS_FreeValue(instance.Context, value);
            instance.Modules.clear();
            instance.DependencyTimes.clear();
            instance.MainPath = filename;
            instance.MainTime = filename.empty() ? std::filesystem::file_time_type::min() : fileTime(filename);
            instance.CurrentDirectory = filename.empty() ? runtimeQuickJSScriptDirectory() : filename.parent_path();
            std::string wrapped = "(function(q){\n\"use strict\";\n" + source + "\n})";
            auto& runtime = workspace();
            runtime.begin(script.TimeoutMs);
            JSValue function = JS_Eval(instance.Context, wrapped.c_str(), wrapped.size(), filename.empty() ? "quartz-runtime.js" : filename.string().c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
            runtime.end();
            if (JS_IsException(function))
            {
                if (runtime.Execution.Interrupted)
                {
                    JSValue exception = JS_GetException(instance.Context);
                    JS_FreeValue(instance.Context, exception);
                    ++script.TimeoutCount;
                    error = "QuickJS compile timed out";
                }
                else error = exceptionText(instance.Context);
                return false;
            }
            if (!JS_IsFunction(instance.Context, function)) { JS_FreeValue(instance.Context, function); error = "QuickJS source did not compile to a function"; return false; }
            instance.Function = function;
            ++script.CompileCount;
            return true;
        }

        JSValue registerObject(JSContext* ctx, const user_regs_struct& regs)
        {
            JSValue object = JS_NewObject(ctx);
#define QUARTZ_REG(name) JS_SetPropertyStr(ctx, object, #name, JS_NewBigUint64(ctx, static_cast<std::uint64_t>(regs.name)))
            QUARTZ_REG(rax); QUARTZ_REG(rbx); QUARTZ_REG(rcx); QUARTZ_REG(rdx); QUARTZ_REG(rsi); QUARTZ_REG(rdi); QUARTZ_REG(rbp); QUARTZ_REG(rsp);
            QUARTZ_REG(r8); QUARTZ_REG(r9); QUARTZ_REG(r10); QUARTZ_REG(r11); QUARTZ_REG(r12); QUARTZ_REG(r13); QUARTZ_REG(r14); QUARTZ_REG(r15);
            QUARTZ_REG(rip); QUARTZ_REG(eflags); QUARTZ_REG(orig_rax); QUARTZ_REG(fs_base); QUARTZ_REG(gs_base);
#undef QUARTZ_REG
            return object;
        }

        bool dispatchBuiltInEvents(Instance& instance, const RuntimeSignalContext& signal, std::string& error)
        {
            if (wantsEvent(instance, "tick"))
            {
                JSValue event = makeEvent(instance.Context, "tick", signal.Time);
                JS_SetPropertyStr(instance.Context, event, "deltaTime", JS_NewFloat64(instance.Context, signal.DeltaTime));
                if (!dispatchEvent(instance, "tick", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                JS_FreeValue(instance.Context, event);
            }
            if (instance.PreviousShader.empty()) instance.PreviousShader = signal.CurrentShaderId;
            else if (instance.PreviousShader != signal.CurrentShaderId)
            {
                if (wantsEvent(instance, "shader.changed"))
                {
                    JSValue event = makeEvent(instance.Context, "shader.changed", signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "previous", JS_NewString(instance.Context, instance.PreviousShader.c_str()));
                    JS_SetPropertyStr(instance.Context, event, "current", JS_NewString(instance.Context, signal.CurrentShaderId.c_str()));
                    if (!dispatchEvent(instance, "shader.changed", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event);
                }
                instance.PreviousShader = signal.CurrentShaderId;
            }
            if (!instance.KeysInitialized)
            {
                instance.PreviousKeys = signal.Keys.Down;
                instance.PreviousCaps = signal.Keys.CapsLockActive;
                instance.PreviousScroll = signal.Keys.ScrollLockActive;
                instance.KeysInitialized = true;
            }
            else
            {
                const bool keyEvents = wantsEvent(instance, "key.down") || wantsEvent(instance, "key.up") || wantsEvent(instance, "key.changed");
                if (keyEvents)
                {
                    for (std::size_t i = 0; i < MatrixSize; ++i)
                    {
                        const bool down = signal.Keys.Down[i] > 0.5f;
                        const bool previous = instance.PreviousKeys[i] > 0.5f;
                        if (down == previous) continue;
                        const char* type = down ? "key.down" : "key.up";
                        JSValue event = makeEvent(instance.Context, type, signal.Time);
                        JS_SetPropertyStr(instance.Context, event, "index", JS_NewUint32(instance.Context, static_cast<std::uint32_t>(i)));
                        JS_SetPropertyStr(instance.Context, event, "row", JS_NewUint32(instance.Context, static_cast<std::uint32_t>(i / Columns)));
                        JS_SetPropertyStr(instance.Context, event, "column", JS_NewUint32(instance.Context, static_cast<std::uint32_t>(i % Columns)));
                        JS_SetPropertyStr(instance.Context, event, "down", JS_NewBool(instance.Context, down));
                        if (wantsEvent(instance, type) && !dispatchEvent(instance, type, event, error)) { JS_FreeValue(instance.Context, event); return false; }
                        if (wantsEvent(instance, "key.changed") && !dispatchEvent(instance, "key.changed", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                        JS_FreeValue(instance.Context, event);
                    }
                }
                instance.PreviousKeys = signal.Keys.Down;
                if (instance.PreviousCaps != signal.Keys.CapsLockActive && wantsEvent(instance, "lock.changed"))
                {
                    JSValue event = makeEvent(instance.Context, "lock.changed", signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "key", JS_NewString(instance.Context, "caps"));
                    JS_SetPropertyStr(instance.Context, event, "active", JS_NewBool(instance.Context, signal.Keys.CapsLockActive));
                    if (!dispatchEvent(instance, "lock.changed", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event);
                }
                if (instance.PreviousScroll != signal.Keys.ScrollLockActive && wantsEvent(instance, "lock.changed"))
                {
                    JSValue event = makeEvent(instance.Context, "lock.changed", signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "key", JS_NewString(instance.Context, "scroll"));
                    JS_SetPropertyStr(instance.Context, event, "active", JS_NewBool(instance.Context, signal.Keys.ScrollLockActive));
                    if (!dispatchEvent(instance, "lock.changed", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event);
                }
                instance.PreviousCaps = signal.Keys.CapsLockActive;
                instance.PreviousScroll = signal.Keys.ScrollLockActive;
            }
            const bool processEvents = wantsEvent(instance, "process.started") || wantsEvent(instance, "process.stopped");
            if (processEvents && signal.Time >= instance.NextProcessPoll)
            {
                instance.NextProcessPoll = signal.Time + 0.25;
                std::unordered_map<pid_t, std::string> current;
                for (const auto& process : enumerateRuntimeProcesses()) current.emplace(process.Pid, process.Name);
                if (instance.ProcessesInitialized)
                {
                    if (wantsEvent(instance, "process.started"))
                        for (const auto& [pid, name] : current) if (!instance.Processes.contains(pid))
                        {
                            JSValue event = makeEvent(instance.Context, "process.started", signal.Time);
                            JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, pid));
                            JS_SetPropertyStr(instance.Context, event, "name", JS_NewString(instance.Context, name.c_str()));
                            if (!dispatchEvent(instance, "process.started", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                            JS_FreeValue(instance.Context, event);
                        }
                    if (wantsEvent(instance, "process.stopped"))
                        for (const auto& [pid, name] : instance.Processes) if (!current.contains(pid))
                        {
                            JSValue event = makeEvent(instance.Context, "process.stopped", signal.Time);
                            JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, pid));
                            JS_SetPropertyStr(instance.Context, event, "name", JS_NewString(instance.Context, name.c_str()));
                            if (!dispatchEvent(instance, "process.stopped", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                            JS_FreeValue(instance.Context, event);
                        }
                }
                instance.Processes = std::move(current);
                instance.ProcessesInitialized = true;
            }
            if (wantsEvent(instance, "breakpoint.hit"))
            {
                if (const auto hit = executionProbe().hit(); hit && hit->Time != instance.LastBreakpointHitTime)
                {
                    instance.LastBreakpointHitTime = hit->Time;
                    JSValue event = makeEvent(instance.Context, "breakpoint.hit", signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, hit->Pid));
                    JS_SetPropertyStr(instance.Context, event, "tid", JS_NewInt32(instance.Context, hit->Tid));
                    JS_SetPropertyStr(instance.Context, event, "address", JS_NewBigUint64(instance.Context, static_cast<std::uint64_t>(hit->Address)));
                    if (hit->HasRegisters) JS_SetPropertyStr(instance.Context, event, "registers", registerObject(instance.Context, hit->Registers));
                    if (!dispatchEvent(instance, "breakpoint.hit", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event);
                }
            }
            if (instance.Script)
            {
                runtimeRefreshQuickJSSignatureScans(*instance.Script);
                for (auto& scan : instance.Script->SignatureScans)
                {
                    if (!scan.Finished || scan.CompletionDelivered) continue;
                    const char* type = scan.Found ? "signature.found" : scan.Cancelled ? "signature.cancelled" : !scan.Error.empty() ? "signature.error" : "signature.not-found";
                    JSValue event = makeEvent(instance.Context, type, signal.Time);
                    JS_SetPropertyStr(instance.Context, event, "id", JS_NewBigUint64(instance.Context, scan.Id)); JS_SetPropertyStr(instance.Context, event, "pid", JS_NewInt32(instance.Context, scan.Pid)); JS_SetPropertyStr(instance.Context, event, "pattern", JS_NewString(instance.Context, scan.Pattern.c_str()));
                    JS_SetPropertyStr(instance.Context, event, "scannedBytes", JS_NewBigUint64(instance.Context, scan.ScannedBytes)); JS_SetPropertyStr(instance.Context, event, "totalBytes", JS_NewBigUint64(instance.Context, scan.TotalBytes)); JS_SetPropertyStr(instance.Context, event, "durationSeconds", JS_NewFloat64(instance.Context, scan.DurationSeconds)); JS_SetPropertyStr(instance.Context, event, "averageMiBs", JS_NewFloat64(instance.Context, scan.AverageMiBs));
                    if (scan.Found) JS_SetPropertyStr(instance.Context, event, "address", JS_NewBigUint64(instance.Context, static_cast<std::uint64_t>(scan.MatchAddress)));
                    if (!scan.Error.empty()) JS_SetPropertyStr(instance.Context, event, "error", JS_NewString(instance.Context, scan.Error.c_str()));
                    if (wantsEvent(instance, type) && !dispatchEvent(instance, type, event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    if (wantsEvent(instance, "signature.finished") && !dispatchEvent(instance, "signature.finished", event, error)) { JS_FreeValue(instance.Context, event); return false; }
                    JS_FreeValue(instance.Context, event); scan.CompletionDelivered = true;
                }
            }
            return true;
        }

        void snapshotState(Instance& instance, RuntimeScript& script) noexcept
        {
            if (!JS_IsFunction(instance.Context, instance.SnapshotFunction)) return;
            JSValue state = JS_GetPropertyStr(instance.Context, instance.Api, "state"); JSValue argument = JS_DupValue(instance.Context, state); JSValue result = JS_Call(instance.Context, instance.SnapshotFunction, JS_UNDEFINED, 1, &argument); JS_FreeValue(instance.Context, argument); JS_FreeValue(instance.Context, state);
            if (JS_IsException(result)) { JSValue exception = JS_GetException(instance.Context); JS_FreeValue(instance.Context, exception); JS_FreeValue(instance.Context, result); return; }
            if (const char* text = JS_ToCString(instance.Context, result)) { script.StateSnapshot = text; JS_FreeCString(instance.Context, text); }
            JS_FreeValue(instance.Context, result);
        }

        bool persistStorage(Instance& instance, RuntimeScript& script, JavaScriptRuntime& javascript, std::string& error)
        {
            JSValue storage = JS_GetPropertyStr(instance.Context, instance.Api, "storage");
            JSValue json = JS_JSONStringify(instance.Context, storage, JS_UNDEFINED, JS_UNDEFINED);
            JS_FreeValue(instance.Context, storage);
            if (JS_IsException(json)) { error = "q.storage must contain only JSON-serializable values: " + exceptionText(instance.Context); return false; }
            const char* text = JS_ToCString(instance.Context, json);
            if (!text) { JS_FreeValue(instance.Context, json); error = exceptionText(instance.Context); return false; }
            if (script.PersistentStateJson != text) { script.PersistentStateJson = text; javascript.markChanged(); }
            JS_FreeCString(instance.Context, text);
            JS_FreeValue(instance.Context, json);
            return true;
        }

        bool evaluate(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)
        {
            auto& runtime = workspace();
            const bool watchExternal = javascript.settings().ExternalHotReload && script.HotReload;
            auto existing = runtime.Instances.find(script.Id);
            if (existing != runtime.Instances.end() && watchExternal && dependenciesChanged(*existing->second)) { javascript.clearOutput(script.Id); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }
            bool reuseExternal = false;
            std::string source;
            std::filesystem::path filename;
            if (script.External)
            {
                filename = script.Path;
                if (filename.is_relative()) filename = runtimeQuickJSScriptDirectory() / filename;
                existing = runtime.Instances.find(script.Id);
                if (existing != runtime.Instances.end() && watchExternal && (existing->second->MainPath != filename || existing->second->MainTime != fileTime(filename))) { javascript.clearOutput(script.Id); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }
                reuseExternal = existing != runtime.Instances.end() && JS_IsFunction(existing->second->Context, existing->second->Function);
                if (!reuseExternal)
                {
                    std::string readError;
                    if (!loadFile(filename, source, readError)) { script.Status = readError; appendLog(script, signal.Time, RuntimeScriptLogLevel::Error, readError); return false; }
                }
            }
            else source = script.Source;
            Instance* instance = createInstance(javascript, script, signal, shader, output, keyboard);
            if (!instance) { script.Status = "could not create QuickJS runtime context"; return false; }
            instance->JavaScript = &javascript;
            instance->Script = &script;
            instance->SignalContext = &signal;
            instance->Keyboard = &keyboard;
            instance->Shader = &shader;
            instance->Output = &output;
            std::string error;
            if (!reuseExternal && !compile(*instance, script, source, filename, error)) { script.Status = std::move(error); return false; }
            JS_SetPropertyStr(instance->Context, instance->Api, "time", JS_NewFloat64(instance->Context, signal.Time));
            JS_SetPropertyStr(instance->Context, instance->Api, "deltaTime", JS_NewFloat64(instance->Context, signal.DeltaTime));
            JS_SetPropertyStr(instance->Context, instance->Api, "id", JS_NewBigUint64(instance->Context, script.Id));
            JS_SetPropertyStr(instance->Context, instance->Api, "name", JS_NewString(instance->Context, script.Name));
            const auto started = std::chrono::steady_clock::now();
            runtime.begin(script.TimeoutMs);
            if (!dispatchBuiltInEvents(*instance, signal, error))
            {
                runtime.end();
                script.LastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                script.Status = std::move(error);
                return false;
            }
            JSValue argument = JS_DupValue(instance->Context, instance->Api);
            JSValue result = JS_Call(instance->Context, instance->Function, JS_UNDEFINED, 1, &argument);
            JS_FreeValue(instance->Context, argument);
            ++script.RunCount;
            if (JS_IsException(result))
            {
                runtime.end();
                script.LastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                if (runtime.Execution.Interrupted)
                {
                    JSValue exception = JS_GetException(instance->Context); JS_FreeValue(instance->Context, exception); ++script.TimeoutCount;
                    script.Status = "QuickJS execution timed out after " + std::to_string(script.TimeoutMs) + " ms";
                }
                else script.Status = exceptionText(instance->Context);
                return false;
            }
            JS_FreeValue(instance->Context, result);
            if (instance->FirstExecution)
            {
                const char* type = instance->Reloaded ? "script.reload" : "script.loaded";
                if (wantsEvent(*instance, type))
                {
                    JSValue event = makeEvent(instance->Context, type, signal.Time);
                    JS_SetPropertyStr(instance->Context, event, "id", JS_NewBigUint64(instance->Context, script.Id));
                    JS_SetPropertyStr(instance->Context, event, "name", JS_NewString(instance->Context, script.Name));
                    if (!dispatchEvent(*instance, type, event, error)) { JS_FreeValue(instance->Context, event); runtime.end(); script.Status = std::move(error); return false; }
                    JS_FreeValue(instance->Context, event);
                }
                instance->FirstExecution = false;
            }
            snapshotState(*instance, script);
            if (!persistStorage(*instance, script, javascript, error)) { runtime.end(); script.Status = std::move(error); return false; }
            runtime.end();
            script.LastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (runtime.Execution.Interrupted) { ++script.TimeoutCount; script.Status = "QuickJS execution timed out after " + std::to_string(script.TimeoutMs) + " ms"; return false; }
            script.Dependencies.clear();
            for (const auto& [path, _] : instance->DependencyTimes) script.Dependencies.push_back(path);
            std::ranges::sort(script.Dependencies);
            script.Status = "running";
            return true;
        }
    }

    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)
    {
        std::vector<RuntimeScript*> order;
        order.reserve(javascript.scripts().size());
        for (auto& script : javascript.scripts()) order.push_back(&script);
        std::ranges::stable_sort(order, [](const RuntimeScript* a, const RuntimeScript* b) { if (a->Order != b->Order) return a->Order < b->Order; return a->Id < b->Id; });
        for (RuntimeScript* script : order)
        {
            if (!script->Enabled)
            {
                if (script->Status != "disabled") { runtimeResetWorkspaceScript(script->Id); script->Status = "disabled"; }
                javascript.clearOutput(script->Id);
                continue;
            }
            if (context.Time < script->NextUpdate) continue;
            const float updateHz = std::clamp(script->UpdateHz, 0.5f, 500.0f);
            script->NextUpdate = context.Time + 1.0 / updateHz;
            evaluate(javascript, *script, context, shader, javascript.outputFor(script->Id), keyboard);
        }
        javascript.rebuildOutput();
        return javascript.output();
    }

    void runtimeResetWorkspaceScript(const std::uint64_t scriptId) noexcept
    {
        auto& runtime = workspace();
        runtime.Instances.erase(scriptId);
        if (runtime.Runtime) JS_RunGC(runtime.Runtime);
    }

    void runtimeReloadAllWorkspaceScripts() noexcept
    {
        auto& runtime = workspace();
        runtime.Instances.clear();
        if (runtime.Runtime) JS_RunGC(runtime.Runtime);
    }

    std::filesystem::path runtimeQuickJSScriptDirectory()
    {
        const auto path = settingsPath().parent_path() / "scripts";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }
}
