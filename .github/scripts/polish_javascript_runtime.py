from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def read(path): return (ROOT / path).read_text()
def write(path, text): (ROOT / path).write_text(text)
def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1: raise RuntimeError(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

# Complete RuntimeSignalContext is needed by q.input lock-state helpers.
path = "src/runtime/QuickJSApi.cpp"
text = read(path)
text = replace_once(text, '#include "quartz/client/input/Input.hpp"\n', '#include "quartz/client/input/Input.hpp"\n#include "quartz/client/runtime/RuntimeTypes.hpp"\n', "QuickJSApi RuntimeTypes include")

# Linux evdev letter keycodes are keyboard-position codes, not alphabetically contiguous.
old = '''            if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') { key = static_cast<std::uint16_t>(KEY_A + (name[0] - 'A')); return true; }\n'''
new = '''            static constexpr std::pair<char, std::uint16_t> Letters[] = {{'A',KEY_A},{'B',KEY_B},{'C',KEY_C},{'D',KEY_D},{'E',KEY_E},{'F',KEY_F},{'G',KEY_G},{'H',KEY_H},{'I',KEY_I},{'J',KEY_J},{'K',KEY_K},{'L',KEY_L},{'M',KEY_M},{'N',KEY_N},{'O',KEY_O},{'P',KEY_P},{'Q',KEY_Q},{'R',KEY_R},{'S',KEY_S},{'T',KEY_T},{'U',KEY_U},{'V',KEY_V},{'W',KEY_W},{'X',KEY_X},{'Y',KEY_Y},{'Z',KEY_Z}};\n            if (name.size() == 1) for (const auto& [letter, code] : Letters) if (name[0] == letter) { key = code; return true; }\n'''
text = replace_once(text, old, new, "evdev letter mapping")
write(path, text)

# Per-script outputs: disabling/reloading a script must release only that script's sticky overrides.
path = "src/runtime/QuickJSWorkspace.cpp"
text = read(path)
text = replace_once(text,
'            if (existing != runtime.Instances.end() && watchExternal && dependenciesChanged(*existing->second)) { runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }',
'            if (existing != runtime.Instances.end() && watchExternal && dependenciesChanged(*existing->second)) { javascript.clearOutput(script.Id); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }', "dependency reload output release")
text = replace_once(text,
'                if (existing != runtime.Instances.end() && watchExternal && (existing->second->MainPath != filename || existing->second->MainTime != fileTime(filename))) { runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }',
'                if (existing != runtime.Instances.end() && watchExternal && (existing->second->MainPath != filename || existing->second->MainTime != fileTime(filename))) { javascript.clearOutput(script.Id); runtime.Instances.erase(existing); ++script.ReloadCount; existing = runtime.Instances.end(); }', "root reload output release")
text = replace_once(text,
'            instance->Reloaded = script.ReloadCount > 0;\n            instance->Context = JS_NewContext(runtime.Runtime);',
'            instance->Reloaded = script.ReloadCount > 0;\n            if (const auto oldHit = executionProbe().hit()) instance->LastBreakpointHitTime = oldHit->Time;\n            instance->Context = JS_NewContext(runtime.Runtime);', "ignore stale breakpoint hit after reload")
old_scheduler = '''        for (RuntimeScript* script : order)\n        {\n            if (!script->Enabled || context.Time < script->NextUpdate) continue;\n            const float updateHz = std::clamp(script->UpdateHz, 0.5f, 500.0f);\n            script->NextUpdate = context.Time + 1.0 / updateHz;\n            evaluate(javascript, legacy, *script, context, shader, javascript.output(), keyboard);\n        }\n        return javascript.output();\n'''
new_scheduler = '''        for (RuntimeScript* script : order)\n        {\n            if (!script->Enabled)\n            {\n                if (script->Status != "disabled") { runtimeResetWorkspaceScript(script->Id); script->Status = "disabled"; }\n                javascript.clearOutput(script->Id);\n                continue;\n            }\n            if (context.Time < script->NextUpdate) continue;\n            const float updateHz = std::clamp(script->UpdateHz, 0.5f, 500.0f);\n            script->NextUpdate = context.Time + 1.0 / updateHz;\n            evaluate(javascript, legacy, *script, context, shader, javascript.outputFor(script->Id), keyboard);\n        }\n        javascript.rebuildOutput();\n        return javascript.output();\n'''
text = replace_once(text, old_scheduler, new_scheduler, "per-script output scheduler")
write(path, text)

# UI reload/reset/edit operations release the corresponding script's runtime output as part of lifecycle reset.
path = "src/ui/pages/JavaScriptPage.cpp"
text = read(path)
text = replace_once(text,
'if (ImGui::Button("Reload all")) { runtimeReloadAllWorkspaceScripts(); for (auto& script : javascript.scripts()) ++script.ReloadCount; status = "all JavaScript contexts reloaded"; }',
'if (ImGui::Button("Reload all")) { runtimeReloadAllWorkspaceScripts(); javascript.clearOutputs(); for (auto& script : javascript.scripts()) ++script.ReloadCount; status = "all JavaScript contexts reloaded"; }', "reload all output release")
text = replace_once(text,
'if (ImGui::SmallButton("Reload")) { runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; }',
'if (ImGui::SmallButton("Reload")) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }', "script reload output release")
text = replace_once(text,
'if (ImGui::SmallButton("Reset persistent storage")) { script.PersistentStateJson = "{}"; runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; localChanged = true; }',
'if (ImGui::SmallButton("Reset persistent storage")) { script.PersistentStateJson = "{}"; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; localChanged = true; }', "storage reset output release")
text = replace_once(text,
'if (oldLegacyBridge != script.LegacyBridge) { runtimeResetWorkspaceScript(script.Id); ++script.ReloadCount; }',
'if (oldLegacyBridge != script.LegacyBridge) { runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); ++script.ReloadCount; }', "bridge toggle output release")
text = replace_once(text,
'if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); localChanged = true; }',
'if (edited != editorState.Synced) { script.Source = edited; editorState.Synced = script.Source; runtimeResetWorkspaceScript(script.Id); javascript.clearOutput(script.Id); localChanged = true; }', "inline edit output release")
write(path, text)

# q.runtime.shader and shaderPreset own the same logical output slot; newest one wins cleanly.
path = "src/runtime/QuickJSGraphApi.cpp"
text = read(path)
text = replace_once(text,
'JSValue runtimeShader(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;std::string id;if(!toString(ctx,argv[0],id))return JS_EXCEPTION;if(!findShaderPresetById(id))return JS_ThrowReferenceError(ctx,"unknown shader id: %s",id.c_str());s->Output->ShaderId=id;if(argc>1){double v=0;if(JS_ToFloat64(ctx,&v,argv[1])<0)return JS_EXCEPTION;s->Output->ShaderTransitionSeconds=std::clamp(static_cast<float>(v),0.0f,10.0f);}return JS_TRUE;}',
'JSValue runtimeShader(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);if(!s||!s->Output||argc<1)return JS_FALSE;std::string id;if(!toString(ctx,argv[0],id))return JS_EXCEPTION;if(!findShaderPresetById(id))return JS_ThrowReferenceError(ctx,"unknown shader id: %s",id.c_str());s->Output->ShaderId=id;s->Output->ShaderPresetIndex.reset();if(argc>1){double v=0;if(JS_ToFloat64(ctx,&v,argv[1])<0)return JS_EXCEPTION;s->Output->ShaderTransitionSeconds=std::clamp(static_cast<float>(v),0.0f,10.0f);}return JS_TRUE;}', "runtime shader exclusivity")
text = replace_once(text,
'JSValue runtimePreset(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);std::int32_t p=0;if(!s||!s->Output||argc<1||JS_ToInt32(ctx,&p,argv[0])<0)return JS_FALSE;if(p<1||p>static_cast<int>(ShaderPresets.size()))return JS_ThrowRangeError(ctx,"shader preset out of range");s->Output->ShaderPresetIndex=p;if(argc>1){double v=0;if(JS_ToFloat64(ctx,&v,argv[1])<0)return JS_EXCEPTION;s->Output->ShaderTransitionSeconds=std::clamp(static_cast<float>(v),0.0f,10.0f);}return JS_TRUE;}',
'JSValue runtimePreset(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*s=state(ctx);std::int32_t p=0;if(!s||!s->Output||argc<1||JS_ToInt32(ctx,&p,argv[0])<0)return JS_FALSE;if(p<1||p>static_cast<int>(ShaderPresets.size()))return JS_ThrowRangeError(ctx,"shader preset out of range");s->Output->ShaderPresetIndex=p;s->Output->ShaderId.reset();if(argc>1){double v=0;if(JS_ToFloat64(ctx,&v,argv[1])<0)return JS_EXCEPTION;s->Output->ShaderTransitionSeconds=std::clamp(static_cast<float>(v),0.0f,10.0f);}return JS_TRUE;}', "runtime preset exclusivity")
write(path, text)

print("JavaScript runtime polish patches applied")
