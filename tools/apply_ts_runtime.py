from pathlib import Path


def once(text: str, old: str, new: str, name: str) -> str:
    if old not in text:
        raise RuntimeError(f"{name}: anchor not found")
    return text.replace(old, new, 1)


def between(text: str, start: str, end: str, replacement: str, name: str) -> str:
    a = text.find(start)
    if a < 0:
        raise RuntimeError(f"{name}: start not found")
    b = text.find(end, a)
    if b < 0:
        raise RuntimeError(f"{name}: end not found")
    return text[:a] + replacement + text[b:]


# Runtime script property model.
p = Path("include/quartz/client/runtime/RuntimeTypes.hpp")
t = p.read_text()
anchor = "    struct RuntimeScript\n    {\n"
props = '''    enum class RuntimeScriptPropertyType : std::uint8_t
    {
        String, Boolean, Int32, UInt32, Float32, Float64, Shader, File, Directory, Key, Enum
    };

    struct RuntimeScriptProperty
    {
        std::string Id;
        std::string Label;
        std::string Group;
        std::string Description;
        RuntimeScriptPropertyType Type = RuntimeScriptPropertyType::String;
        std::string StringValue;
        std::string DefaultString;
        double NumberValue = 0.0;
        double DefaultNumber = 0.0;
        double Min = 0.0;
        double Max = 0.0;
        double Step = 0.1;
        bool BoolValue = false;
        bool DefaultBool = false;
        bool KeyIsNumber = false;
        bool DefaultKeyIsNumber = false;
        bool HasMin = false;
        bool HasMax = false;
        std::vector<std::string> EnumValues;
        std::uint64_t Revision = 1;
    };

'''
t = once(t, anchor, props + anchor, "property model")
t = once(t, '        char Name[64] = "JavaScript";\n', '        char Name[64] = "Script";\n        std::string StableId;\n', "script stable id")
t = once(t, '        std::string PersistentStateJson = "{}";\n        bool HotReload = true;\n', '        std::string PersistentStateJson = "{}";\n        std::vector<RuntimeScriptProperty> Properties;\n        bool HotReload = true;\n        bool LoadedFromConfig = false;\n        bool DefaultsApplied = false;\n        bool ReloadRequested = false;\n', "script SDK state")
p.write_text(t)


# Public runtime reset reasons.
p = Path("include/quartz/client/runtime/QuickJS.hpp")
t = p.read_text()
t = once(t, '    void runtimeResetWorkspaceScript(std::uint64_t scriptId) noexcept;\n    void runtimeReloadAllWorkspaceScripts() noexcept;\n', '    void runtimeResetWorkspaceScript(std::uint64_t scriptId, std::string_view reason = "reload") noexcept;\n    void runtimeReloadAllWorkspaceScripts(std::string_view reason = "reload") noexcept;\n', "workspace lifecycle declarations")
p.write_text(t)


# Script persistence keeps v1/v2 compatibility, while source metadata becomes native TS SDK metadata.
p = Path("src/runtime/JavaScriptRuntime.cpp")
t = p.read_text()
t = once(t, '        runtimeReloadAllWorkspaceScripts();\n        save();\n', '        runtimeReloadAllWorkspaceScripts("shutdown");\n        save();\n', "shutdown dispose")
t = once(t, '        std::snprintf(script.Name, sizeof(script.Name), "JavaScript %zu", _scripts.size());\n', '        std::snprintf(script.Name, sizeof(script.Name), "Script %zu", _scripts.size());\n', "script default name")
t = once(t, '        runtimeResetWorkspaceScript(id);\n', '        runtimeResetWorkspaceScript(id, "removed");\n', "removed reason")
t = once(t, '                if (script.PersistentStateJson.empty()) script.PersistentStateJson = "{}";\n                if (script.Id == 0)', '                if (script.PersistentStateJson.empty()) script.PersistentStateJson = "{}";\n                script.LoadedFromConfig = true;\n                if (script.Id == 0)', "loaded config marker")
p.write_text(t)


# QuickJS workspace becomes a real ES-module runtime.
p = Path("src/runtime/QuickJSWorkspace.cpp")
t = p.read_text()
t = once(t, '#include <cstring>\n#include <fstream>\n', '#include <cstring>\n#include <filesystem>\n#include <fstream>\n', "workspace filesystem include")
old_instance = '''        struct Instance : RuntimeQuickJSContext
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

'''
new_instance = '''        struct Instance : RuntimeQuickJSContext
        {
            JSContext* Context = nullptr;
            JSValue Api = JS_UNDEFINED;
            JSValue SnapshotFunction = JS_UNDEFINED;
            std::uint64_t SourceHash = 0;
            std::filesystem::path MainPath;
            std::filesystem::file_time_type MainTime{};
            std::unordered_map<std::string, std::filesystem::file_time_type> DependencyTimes;
            std::unordered_map<std::string, std::uint64_t> PropertyRevisions;
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
            bool ModuleLoaded = false;
            bool FirstExecution = true;
            bool Reloaded = false;

            ~Instance()
            {
                if (Script) runtimeCancelQuickJSSignatureScans(*Script);
                if (!Context) return;
                for (auto& subscription : Subscriptions) JS_FreeValue(Context, subscription.Callback);
                JS_FreeValue(Context, SnapshotFunction);
                JS_FreeValue(Context, Api);
                JS_FreeContext(Context);
            }
        };

        char* moduleNormalize(JSContext* ctx, const char* baseName, const char* moduleName, void* opaque);
        JSModuleDef* moduleLoader(JSContext* ctx, const char* moduleName, void* opaque);

'''
t = once(t, old_instance, new_instance, "workspace instance")
t = once(t, '                JS_SetCanBlock(Runtime, false);\n                JS_SetInterruptHandler(Runtime, [](JSRuntime*, void* opaque)\n', '                JS_SetCanBlock(Runtime, false);\n                JS_SetModuleLoaderFunc(Runtime, moduleNormalize, moduleLoader, this);\n                JS_SetInterruptHandler(Runtime, [](JSRuntime*, void* opaque)\n', "module loader install")

# CommonJS q.import is gone; ES module loading owns dependency resolution now.
start = '        JSValue jsImport(JSContext* ctx, JSValueConst, const int argc, JSValueConst* argv)\n'
end = '        Instance* createInstance(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n'
t = between(t, start, end, '', "remove q.import")
t = t.replace('            JS_SetPropertyStr(instance->Context, instance->Api, "import", JS_NewCFunction(instance->Context, jsImport, "import", 1));\n', '')

# Install standard console globally and the private SDK bridge after storage exists.
storage_anchor = '''            JS_DefinePropertyValueStr(instance->Context, instance->Api, "storage", storage, JS_PROP_ENUMERABLE);
            static constexpr std::string_view SnapshotSource ='''
storage_replace = '''            JS_DefinePropertyValueStr(instance->Context, instance->Api, "storage", storage, JS_PROP_ENUMERABLE);
            runtimeInstallQuickJSSDKNativeApi(instance->Context, instance->Api);
            JSValue global = JS_GetGlobalObject(instance->Context); JSValue standardConsole = JS_GetPropertyStr(instance->Context, instance->Api, "console"); JS_SetPropertyStr(instance->Context, global, "console", standardConsole); JS_FreeValue(instance->Context, global);
            static constexpr std::string_view SnapshotSource ='''
t = once(t, storage_anchor, storage_replace, "SDK native install")
t = t.replace('"q.storage"', '"Script.storage"')

# Replace function-body compilation with TS -> ES module compilation and a virtual module loader.
compile_start = '        bool dependenciesChanged(const Instance& instance)\n'
compile_end = '        JSValue registerObject(JSContext* ctx, const user_regs_struct& regs)\n'
new_compile = r'''        bool dependenciesChanged(const Instance& instance)
        {
            for (const auto& [path, time] : instance.DependencyTimes) if (fileTime(path) != time) return true;
            return false;
        }

        bool typeScriptPath(const std::filesystem::path& path)
        {
            const auto extension = path.extension().string(); return extension == ".ts" || extension == ".mts" || extension == ".cts";
        }

        std::filesystem::path normalizedModulePath(const std::filesystem::path& base, const std::filesystem::path& requested)
        {
            std::filesystem::path path = requested.is_absolute() ? requested : base / requested; std::error_code ec;
            if (!path.has_extension())
            {
                const std::filesystem::path candidates[] = {std::filesystem::path(path.string() + ".ts"), std::filesystem::path(path.string() + ".js"), path / "index.ts", path / "index.js"};
                for (const auto& candidate : candidates) if (std::filesystem::is_regular_file(candidate, ec)) { path = candidate; break; }
            }
            auto canonical = std::filesystem::weakly_canonical(path, ec); return ec ? path.lexically_normal() : canonical;
        }

        int nativeModuleInit(JSContext* ctx, JSModuleDef* module)
        {
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx)); if (!instance) return -1; return JS_SetModuleExport(ctx, module, "api", JS_DupValue(ctx, instance->Api));
        }

        char* moduleNormalize(JSContext* ctx, const char* baseName, const char* moduleName, void*)
        {
            const std::string_view requested = moduleName ? moduleName : "";
            if (requested == "@quartz/client" || requested == "@quartz/native") return js_strdup(ctx, moduleName);
            if (requested.empty()) { JS_ThrowReferenceError(ctx, "empty module specifier"); return nullptr; }
            const std::filesystem::path requestPath(requested); if (!requestPath.is_absolute() && !requested.starts_with("./") && !requested.starts_with("../")) { JS_ThrowReferenceError(ctx, "unsupported package module '%s'", moduleName); return nullptr; }
            const std::string_view base = baseName ? baseName : ""; if (base.starts_with("@quartz/")) { JS_ThrowReferenceError(ctx, "relative import '%s' is not valid from %s", moduleName, baseName); return nullptr; }
            const std::filesystem::path basePath(base); const auto resolved = normalizedModulePath(basePath.empty() ? runtimeQuickJSScriptDirectory() : basePath.parent_path(), requestPath); const std::string value = resolved.string(); return js_strdup(ctx, value.c_str());
        }

        JSModuleDef* moduleLoader(JSContext* ctx, const char* moduleName, void*)
        {
            auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx)); if (!instance) { JS_ThrowInternalError(ctx, "Quartz module loaded without a script context"); return nullptr; }
            if (std::string_view(moduleName) == "@quartz/native")
            {
                JSModuleDef* module = JS_NewCModule(ctx, moduleName, nativeModuleInit); if (!module) return nullptr; if (JS_AddModuleExport(ctx, module, "api") < 0) return nullptr; return module;
            }
            std::string source, compiled, error;
            if (std::string_view(moduleName) == "@quartz/client") source.assign(runtimeQuickJSSDKModuleSource());
            else
            {
                const std::filesystem::path path(moduleName); if (!loadFile(path, source, error)) { JS_ThrowReferenceError(ctx, "%s", error.c_str()); return nullptr; }
                instance->DependencyTimes[path.string()] = fileTime(path);
                if (typeScriptPath(path) && !runtimeTranspileTypeScript(source, compiled, error)) { JS_ThrowSyntaxError(ctx, "%s", error.c_str()); return nullptr; }
                if (typeScriptPath(path)) source.swap(compiled);
            }
            JSValue value = JS_Eval(ctx, source.data(), source.size(), moduleName, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(value)) return nullptr; JSModuleDef* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value)); JS_FreeValue(ctx, value); return module;
        }

        bool compile(Instance& instance, RuntimeScript& script, const std::string& source, const std::filesystem::path& filename, std::string& error)
        {
            const auto hash = hashText(source); if (instance.SourceHash == hash && instance.ModuleLoaded) return true;
            std::string compiled = source; const std::filesystem::path modulePath = filename.empty() ? runtimeQuickJSScriptDirectory() / ("inline-" + std::to_string(script.Id) + ".ts") : filename;
            if ((filename.empty() || typeScriptPath(filename)) && !runtimeTranspileTypeScript(source, compiled, error)) { error = "TypeScript: " + error; return false; }
            instance.SourceHash = hash; instance.DependencyTimes.clear(); instance.MainPath = modulePath; instance.MainTime = filename.empty() ? std::filesystem::file_time_type::min() : fileTime(filename);
            auto& runtime = workspace(); runtime.begin(script.TimeoutMs);
            JSValue module = JS_Eval(instance.Context, compiled.data(), compiled.size(), modulePath.string().c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(module))
            {
                runtime.end(); if (runtime.Execution.Interrupted) { JSValue exception = JS_GetException(instance.Context); JS_FreeValue(instance.Context, exception); ++script.TimeoutCount; error = "QuickJS compile timed out"; } else error = exceptionText(instance.Context); return false;
            }
            if (JS_ResolveModule(instance.Context, module) < 0) { error = exceptionText(instance.Context); JS_FreeValue(instance.Context, module); runtime.end(); return false; }
            JSValue result = JS_EvalFunction(instance.Context, module); runtime.end();
            if (JS_IsException(result)) { if (runtime.Execution.Interrupted) { JSValue exception = JS_GetException(instance.Context); JS_FreeValue(instance.Context, exception); ++script.TimeoutCount; error = "QuickJS module initialization timed out"; } else error = exceptionText(instance.Context); JS_FreeValue(instance.Context, result); return false; }
            JS_FreeValue(instance.Context, result); instance.ModuleLoaded = true; ++script.CompileCount; return true;
        }

'''
t = between(t, compile_start, compile_end, new_compile, "module compiler")

# Every app frame services async resources; tick remains rate-limited.
t = once(t, '        bool dispatchBuiltInEvents(Instance& instance, const RuntimeSignalContext& signal, std::string& error)\n        {\n            if (wantsEvent(instance, "tick"))\n', '        bool dispatchBuiltInEvents(Instance& instance, const RuntimeSignalContext& signal, const bool emitTick, std::string& error)\n        {\n            if (wantsEvent(instance, "__quartz.frame"))\n            {\n                JSValue event = makeEvent(instance.Context, "__quartz.frame", signal.Time);\n                JS_SetPropertyStr(instance.Context, event, "deltaTime", JS_NewFloat64(instance.Context, signal.DeltaTime));\n                if (!dispatchEvent(instance, "__quartz.frame", event, error)) { JS_FreeValue(instance.Context, event); return false; }\n                JS_FreeValue(instance.Context, event);\n            }\n            if (emitTick && wantsEvent(instance, "tick"))\n', "frame service event")

# Property changes become resource-local events used by Property.on().
property_events_anchor = '''            }
            return true;
        }

        void snapshotState'''
property_events = '''            }
            if (instance.Script)
            {
                for (const auto& property : instance.Script->Properties)
                {
                    auto [it, inserted] = instance.PropertyRevisions.try_emplace(property.Id, property.Revision); if (inserted || it->second == property.Revision) continue; it->second = property.Revision;
                    if (!wantsEvent(instance, "property.changed")) continue;
                    JSValue event = makeEvent(instance.Context, "property.changed", signal.Time); JS_SetPropertyStr(instance.Context, event, "id", JS_NewString(instance.Context, property.Id.c_str()));
                    switch (property.Type)
                    {
                    case RuntimeScriptPropertyType::Boolean: JS_SetPropertyStr(instance.Context, event, "value", JS_NewBool(instance.Context, property.BoolValue)); break;
                    case RuntimeScriptPropertyType::Int32: JS_SetPropertyStr(instance.Context, event, "value", JS_NewInt32(instance.Context, static_cast<std::int32_t>(property.NumberValue))); break;
                    case RuntimeScriptPropertyType::UInt32: JS_SetPropertyStr(instance.Context, event, "value", JS_NewUint32(instance.Context, static_cast<std::uint32_t>(property.NumberValue))); break;
                    case RuntimeScriptPropertyType::Float32: case RuntimeScriptPropertyType::Float64: JS_SetPropertyStr(instance.Context, event, "value", JS_NewFloat64(instance.Context, property.NumberValue)); break;
                    case RuntimeScriptPropertyType::Key: JS_SetPropertyStr(instance.Context, event, "value", property.KeyIsNumber ? JS_NewInt32(instance.Context, static_cast<std::int32_t>(property.NumberValue)) : JS_NewString(instance.Context, property.StringValue.c_str())); break;
                    default: JS_SetPropertyStr(instance.Context, event, "value", JS_NewString(instance.Context, property.StringValue.c_str())); break;
                    }
                    if (!dispatchEvent(instance, "property.changed", event, error)) { JS_FreeValue(instance.Context, event); return false; } JS_FreeValue(instance.Context, event);
                }
            }
            return true;
        }

        void snapshotState'''
t = once(t, property_events_anchor, property_events, "property changed events")

# Persist automatic Property values inside Script.storage without exposing a second persistence system.
persist_anchor = '        bool persistStorage(Instance& instance, RuntimeScript& script, JavaScriptRuntime& javascript, std::string& error)\n'
sync_storage = r'''        void syncPropertyStorage(Instance& instance, const RuntimeScript& script)
        {
            JSValue storage = JS_GetPropertyStr(instance.Context, instance.Api, "storage"); if (!JS_IsObject(storage)) { JS_FreeValue(instance.Context, storage); return; }
            JSValue properties = JS_GetPropertyStr(instance.Context, storage, "__quartzProperties");
            if (!JS_IsObject(properties)) { JS_FreeValue(instance.Context, properties); properties = JS_NewObject(instance.Context); JS_SetPropertyStr(instance.Context, storage, "__quartzProperties", JS_DupValue(instance.Context, properties)); }
            for (const auto& property : script.Properties)
            {
                JSValue value = JS_UNDEFINED;
                switch (property.Type)
                {
                case RuntimeScriptPropertyType::Boolean: value = JS_NewBool(instance.Context, property.BoolValue); break;
                case RuntimeScriptPropertyType::Int32: value = JS_NewInt32(instance.Context, static_cast<std::int32_t>(property.NumberValue)); break;
                case RuntimeScriptPropertyType::UInt32: value = JS_NewUint32(instance.Context, static_cast<std::uint32_t>(property.NumberValue)); break;
                case RuntimeScriptPropertyType::Float32: case RuntimeScriptPropertyType::Float64: value = JS_NewFloat64(instance.Context, property.NumberValue); break;
                case RuntimeScriptPropertyType::Key: value = property.KeyIsNumber ? JS_NewInt32(instance.Context, static_cast<std::int32_t>(property.NumberValue)) : JS_NewString(instance.Context, property.StringValue.c_str()); break;
                default: value = JS_NewString(instance.Context, property.StringValue.c_str()); break;
                }
                JS_SetPropertyStr(instance.Context, properties, property.Id.c_str(), value);
            }
            JS_FreeValue(instance.Context, properties); JS_FreeValue(instance.Context, storage);
        }

'''
t = once(t, persist_anchor, sync_storage + persist_anchor, "property storage sync")
t = t.replace('error = "q.storage must contain only JSON-serializable values: "', 'error = "Script.storage must contain only JSON-serializable values: "')

# Replace per-update function invocation with one-time module lifecycle + async servicing.
eval_start = '        bool evaluate(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard)\n'
eval_end = '    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)\n'
new_eval = r'''        void disposeInstance(Instance& instance, const std::string_view reason) noexcept
        {
            if (!instance.Context || !wantsEvent(instance, "script.dispose")) return; std::string error; const double time = instance.SignalContext ? instance.SignalContext->Time : 0.0;
            JSValue event = makeEvent(instance.Context, "script.dispose", time); JS_SetPropertyStr(instance.Context, event, "reason", JS_NewStringLen(instance.Context, reason.data(), reason.size())); dispatchEvent(instance, "script.dispose", event, error); JS_FreeValue(instance.Context, event);
        }

        void eraseInstance(Workspace& runtime, const std::uint64_t id, const std::string_view reason)
        {
            const auto it = runtime.Instances.find(id); if (it == runtime.Instances.end()) return; disposeInstance(*it->second, reason); runtime.Instances.erase(it);
        }

        bool dispatchInitialize(Instance& instance, RuntimeScript& script, const RuntimeSignalContext& signal, std::string& error)
        {
            for (const auto& property : script.Properties) instance.PropertyRevisions[property.Id] = property.Revision;
            if (wantsEvent(instance, "script.initialize"))
            {
                JSValue event = makeEvent(instance.Context, "script.initialize", signal.Time); if (!dispatchEvent(instance, "script.initialize", event, error)) { JS_FreeValue(instance.Context, event); return false; } JS_FreeValue(instance.Context, event);
            }
            const char* type = instance.Reloaded ? "script.reload" : "script.loaded";
            if (wantsEvent(instance, type))
            {
                JSValue event = makeEvent(instance.Context, type, signal.Time); JS_SetPropertyStr(instance.Context, event, "id", JS_NewBigUint64(instance.Context, script.Id)); JS_SetPropertyStr(instance.Context, event, "name", JS_NewString(instance.Context, script.Name));
                if (!dispatchEvent(instance, type, event, error)) { JS_FreeValue(instance.Context, event); return false; } JS_FreeValue(instance.Context, event);
            }
            instance.FirstExecution = false; return true;
        }

        bool evaluate(JavaScriptRuntime& javascript, RuntimeScript& script, const RuntimeSignalContext& signal, ShaderFramebuffer& shader, RuntimeControlOutput& output, const EvdevKeyboard& keyboard, const bool emitTick)
        {
            auto& runtime = workspace(); const bool watchExternal = javascript.settings().ExternalHotReload && script.HotReload; auto existing = runtime.Instances.find(script.Id);
            if (existing != runtime.Instances.end() && watchExternal && dependenciesChanged(*existing->second)) { javascript.clearOutput(script.Id); disposeInstance(*existing->second, "reload"); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }
            bool reuseExternal = false; std::string source; std::filesystem::path filename;
            if (script.External)
            {
                filename = script.Path; if (filename.is_relative()) filename = runtimeQuickJSScriptDirectory() / filename; existing = runtime.Instances.find(script.Id);
                if (existing != runtime.Instances.end() && watchExternal && (existing->second->MainPath != filename || existing->second->MainTime != fileTime(filename))) { javascript.clearOutput(script.Id); disposeInstance(*existing->second, "reload"); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }
                reuseExternal = existing != runtime.Instances.end() && existing->second->ModuleLoaded;
                if (!reuseExternal) { std::string readError; if (!loadFile(filename, source, readError)) { script.Status = readError; appendLog(script, signal.Time, RuntimeScriptLogLevel::Error, readError); return false; } }
            }
            else source = script.Source;
            Instance* instance = createInstance(javascript, script, signal, shader, output, keyboard); if (!instance) { script.Status = "could not create QuickJS runtime context"; return false; }
            instance->JavaScript = &javascript; instance->Script = &script; instance->SignalContext = &signal; instance->Keyboard = &keyboard; instance->Shader = &shader; instance->Output = &output;
            JS_SetPropertyStr(instance->Context, instance->Api, "time", JS_NewFloat64(instance->Context, signal.Time)); JS_SetPropertyStr(instance->Context, instance->Api, "deltaTime", JS_NewFloat64(instance->Context, signal.DeltaTime)); JS_SetPropertyStr(instance->Context, instance->Api, "id", JS_NewBigUint64(instance->Context, script.Id)); JS_SetPropertyStr(instance->Context, instance->Api, "name", JS_NewString(instance->Context, script.Name));
            std::string error; const bool wasLoaded = instance->ModuleLoaded; if (!reuseExternal && !compile(*instance, script, source, filename, error)) { script.Status = std::move(error); return false; }
            const auto started = std::chrono::steady_clock::now(); runtime.begin(script.TimeoutMs);
            if (!wasLoaded && instance->ModuleLoaded && !dispatchInitialize(*instance, script, signal, error)) { runtime.end(); script.Status = std::move(error); return false; }
            if (!dispatchBuiltInEvents(*instance, signal, emitTick, error)) { runtime.end(); script.LastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); script.Status = std::move(error); return false; }
            if (emitTick) ++script.RunCount; syncPropertyStorage(*instance, script); snapshotState(*instance, script); if (!persistStorage(*instance, script, javascript, error)) { runtime.end(); script.Status = std::move(error); return false; }
            runtime.end(); script.LastMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); if (runtime.Execution.Interrupted) { ++script.TimeoutCount; script.Status = "QuickJS execution timed out after " + std::to_string(script.TimeoutMs) + " ms"; return false; }
            script.Dependencies.clear(); for (const auto& [path, _] : instance->DependencyTimes) script.Dependencies.push_back(path); std::ranges::sort(script.Dependencies); script.Status = "running"; return true;
        }

        void pumpJobs(Workspace& runtime)
        {
            for (int count = 0; count < 1024 && runtime.Runtime && JS_IsJobPending(runtime.Runtime); ++count)
            {
                JSContext* ctx = nullptr; runtime.begin(8.0f); const int result = JS_ExecutePendingJob(runtime.Runtime, &ctx); const bool interrupted = runtime.Execution.Interrupted; runtime.end(); if (result >= 0) continue;
                if (!ctx) continue; auto* instance = static_cast<Instance*>(JS_GetContextOpaque(ctx)); const std::string error = interrupted ? "QuickJS async job timed out" : exceptionText(ctx);
                if (instance && instance->Script) { instance->Script->Status = error; appendLog(*instance->Script, instance->SignalContext ? instance->SignalContext->Time : 0.0, RuntimeScriptLogLevel::Error, error); if (interrupted) ++instance->Script->TimeoutCount; }
            }
        }
    }

'''
t = between(t, eval_start, eval_end, new_eval, "module evaluation")

# Replace public frame scheduler/reset tail, leaving script directory helper intact.
tail_start = '    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)\n'
tail_end = '    std::filesystem::path runtimeQuickJSScriptDirectory()\n'
new_tail = r'''    const RuntimeControlOutput& runtimeEvaluateWorkspaceScripts(JavaScriptRuntime& javascript, const RuntimeSignalContext& context, ShaderFramebuffer& shader, const EvdevKeyboard& keyboard)
    {
        auto& runtime = workspace(); std::vector<RuntimeScript*> order; order.reserve(javascript.scripts().size()); for (auto& script : javascript.scripts()) order.push_back(&script);
        std::ranges::stable_sort(order, [](const RuntimeScript* a, const RuntimeScript* b) { if (a->Order != b->Order) return a->Order < b->Order; return a->Id < b->Id; });
        for (RuntimeScript* script : order)
        {
            if (!script->Enabled)
            {
                if (script->Status != "disabled") { runtimeResetWorkspaceScript(script->Id, "disabled"); script->Status = "disabled"; } javascript.clearOutput(script->Id); continue;
            }
            const bool emitTick = context.Time >= script->NextUpdate;
            if (emitTick) { const float updateHz = std::clamp(script->UpdateHz, 0.5f, 500.0f); script->NextUpdate = context.Time + 1.0 / updateHz; }
            evaluate(javascript, *script, context, shader, javascript.outputFor(script->Id), keyboard, emitTick);
            if (script->ReloadRequested) { script->ReloadRequested = false; runtimeResetWorkspaceScript(script->Id, "reload"); javascript.clearOutput(script->Id); ++script->ReloadCount; }
        }
        pumpJobs(runtime);
        for (RuntimeScript* script : order) if (script->ReloadRequested) { script->ReloadRequested = false; runtimeResetWorkspaceScript(script->Id, "reload"); javascript.clearOutput(script->Id); ++script->ReloadCount; }
        javascript.rebuildOutput(); return javascript.output();
    }

    void runtimeResetWorkspaceScript(const std::uint64_t scriptId, const std::string_view reason) noexcept
    {
        auto& runtime = workspace(); eraseInstance(runtime, scriptId, reason); if (runtime.Runtime) JS_RunGC(runtime.Runtime);
    }

    void runtimeReloadAllWorkspaceScripts(const std::string_view reason) noexcept
    {
        auto& runtime = workspace(); std::vector<std::uint64_t> ids; ids.reserve(runtime.Instances.size()); for (const auto& [id, _] : runtime.Instances) ids.push_back(id); for (const auto id : ids) eraseInstance(runtime, id, reason); if (runtime.Runtime) JS_RunGC(runtime.Runtime);
    }

'''
t = between(t, tail_start, tail_end, new_tail, "workspace scheduler")

if 'JS_IsFunction(existing->second->Context, existing->second->Function)' in t or 'JS_Call(instance->Context, instance->Function' in t or 'q.import' in t:
    raise RuntimeError("old function-body workspace execution survived")
p.write_text(t)


# TypeScript UI: real external source view, generated properties, and centered status indicator.
p = Path("src/ui/pages/JavaScriptPage.cpp")
t = p.read_text()
t = t.replace('(\"script-\" + std::to_string(script.Id) + \".js\")', '("script-" + std::to_string(script.Id) + ".ts")')
t = t.replace('(runtimeQuickJSScriptDirectory() / "script.js").string()', '(runtimeQuickJSScriptDirectory() / "script.ts").string()')
indicator_old = '''        void drawRuntimeIndicator(const RuntimeScript& script)
        {
            const float pulse = 0.62f + 0.38f * static_cast<float>((std::sin(ImGui::GetTime() * 3.6) + 1.0) * 0.5);
            const bool running = script.Enabled && script.Status.starts_with("running"); const bool failed = script.Enabled && !script.Status.empty() && !running && script.Status != "disabled";
            const ImVec4 color = running ? ImVec4(0.18f, 0.86f, 0.95f, pulse) : failed ? ImVec4(0.95f, 0.30f, 0.28f, 0.92f) : ImVec4(0.42f, 0.45f, 0.50f, 0.75f);
            const ImVec2 position = ImGui::GetCursorScreenPos(); const float side = ImGui::GetTextLineHeight() * 0.72f; ImGui::Dummy(ImVec2(side, side)); ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + side, position.y + side), ImGui::ColorConvertFloat4ToU32(color), 2.0f);
            ImGui::SameLine(); ImGui::TextUnformatted(running ? "running" : failed ? "error / waiting" : script.Enabled ? "waiting" : "disabled");
        }
'''
indicator_new = '''        void drawRuntimeIndicator(const RuntimeScript& script)
        {
            const float pulse = 0.62f + 0.38f * static_cast<float>((std::sin(ImGui::GetTime() * 3.6) + 1.0) * 0.5);
            const bool running = script.Enabled && script.Status.starts_with("running"); const bool failed = script.Enabled && !script.Status.empty() && !running && script.Status != "disabled";
            const ImVec4 color = running ? ImVec4(0.18f, 0.86f, 0.95f, pulse) : failed ? ImVec4(0.95f, 0.30f, 0.28f, 0.92f) : ImVec4(0.42f, 0.45f, 0.50f, 0.75f);
            const float rowHeight = ImGui::GetTextLineHeight(); const float side = rowHeight * 0.72f; ImGui::Dummy(ImVec2(side, rowHeight)); const ImVec2 min = ImGui::GetItemRectMin(); const ImVec2 max = ImGui::GetItemRectMax(); const float y = min.y + (max.y - min.y - side) * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, y), ImVec2(min.x + side, y + side), ImGui::ColorConvertFloat4ToU32(color), 2.0f); ImGui::SameLine(); ImGui::TextUnformatted(running ? "running" : failed ? "error / waiting" : script.Enabled ? "waiting" : "disabled");
        }
'''
t = once(t, indicator_old, indicator_new, "center runtime indicator")

property_ui = r'''        bool drawScriptProperty(RuntimeScriptProperty& property)
        {
            const char* label = property.Label.empty() ? property.Id.c_str() : property.Label.c_str(); bool changed = false;
            ImGui::PushID(property.Id.c_str());
            switch (property.Type)
            {
            case RuntimeScriptPropertyType::Boolean: changed = ImGui::Checkbox(label, &property.BoolValue); break;
            case RuntimeScriptPropertyType::Int32:
            case RuntimeScriptPropertyType::UInt32:
            case RuntimeScriptPropertyType::Float32:
            case RuntimeScriptPropertyType::Float64:
            {
                double value = property.NumberValue; const double* min = property.HasMin ? &property.Min : nullptr; const double* max = property.HasMax ? &property.Max : nullptr; const char* format = property.Type == RuntimeScriptPropertyType::Int32 || property.Type == RuntimeScriptPropertyType::UInt32 ? "%.0f" : "%.3f";
                ImGui::SetNextItemWidth(260.0f); if (ImGui::DragScalar(label, ImGuiDataType_Double, &value, static_cast<float>(property.Step), min, max, format)) { if (property.Type == RuntimeScriptPropertyType::Int32 || property.Type == RuntimeScriptPropertyType::UInt32) value = std::round(value); if (property.Type == RuntimeScriptPropertyType::UInt32) value = std::max(value, 0.0); property.NumberValue = value; changed = true; } break;
            }
            case RuntimeScriptPropertyType::Enum:
                if (ImGui::BeginCombo(label, property.StringValue.c_str())) { for (const auto& value : property.EnumValues) { const bool selected = property.StringValue == value; if (ImGui::Selectable(value.c_str(), selected)) { property.StringValue = value; changed = true; } if (selected) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); } break;
            default:
            {
                char value[1024]{}; std::snprintf(value, sizeof(value), "%s", property.StringValue.c_str()); ImGui::SetNextItemWidth(-80.0f); if (ImGui::InputText(label, value, sizeof(value))) { property.StringValue = value; property.KeyIsNumber = false; changed = true; } break;
            }
            }
            if (!property.Description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", property.Description.c_str());
            ImGui::SameLine(); if (ImGui::SmallButton("Reset")) { property.StringValue = property.DefaultString; property.NumberValue = property.DefaultNumber; property.BoolValue = property.DefaultBool; property.KeyIsNumber = property.DefaultKeyIsNumber; changed = true; }
            if (changed) ++property.Revision; ImGui::PopID(); return changed;
        }

        bool drawScriptProperties(RuntimeScript& script)
        {
            if (script.Properties.empty()) return false; bool changed = false; std::string group;
            ImGui::SeparatorText("Properties");
            for (auto& property : script.Properties)
            {
                if (property.Group != group) { group = property.Group; if (!group.empty()) ImGui::TextDisabled("%s", group.c_str()); }
                changed |= drawScriptProperty(property);
            }
            return changed;
        }

'''
t = once(t, '        void drawRuntimeIndicator(const RuntimeScript& script)\n', property_ui + '        void drawRuntimeIndicator(const RuntimeScript& script)\n', "property UI")
order_anchor = '                ImGui::SetNextItemWidth(180.0f); localChanged |= ImGui::InputText("Group", script.Group, sizeof(script.Group)); ImGui::SameLine(); ImGui::SetNextItemWidth(80.0f); localChanged |= ImGui::InputInt("Order", &script.Order);\n'
t = once(t, order_anchor, order_anchor + '                localChanged |= drawScriptProperties(script);\n', "render script properties")
t = t.replace('script.PersistentStateJson = "{}"; runtimeResetWorkspaceScript(script.Id);', 'script.PersistentStateJson = "{}"; script.Properties.clear(); runtimeResetWorkspaceScript(script.Id);')
p.write_text(t)

print("TypeScript runtime migration applied")
