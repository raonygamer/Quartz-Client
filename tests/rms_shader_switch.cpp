#include "quartz/client/Model.hpp"
#include "quartz/client/runtime/JavaScriptRuntime.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include <iostream>

using namespace quartz::client;
namespace
{
    void check(bool ok, const std::string& message)
    {
        if (!ok) throw std::runtime_error(message);
        std::cout << "PASS: " << message << '\n';
    }
    RuntimeScriptProperty& property(RuntimeScript& script, std::string_view id)
    {
        for (auto& value : script.Properties) if (value.Id == id) return value;
        throw std::runtime_error("missing script property: " + std::string(id));
    }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto temporaryRoot = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    const auto config = temporaryRoot / ("quartz-script-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    _wputenv_s(L"QUARTZ_CONFIG_HOME", config.c_str());
    try
    {
        ShaderFramebuffer shader;
        EvdevKeyboard keyboard;
        RuntimeSignalContext signal;
        signal.CurrentShaderId = "builtin.rainbow_equalizer";
        signal.PreviousShaderId = "builtin.neon_triangle";
        signal.Audio.Rms = 0.1f;
        {
            JavaScriptRuntime javascript;
            auto& script = javascript.add();
            script.External = true; script.Path = std::filesystem::absolute(argv[1]).string();
            const auto step = [&]() -> const RuntimeControlOutput& {
                signal.Time += 0.1;
                const auto& output = runtimeEvaluateWorkspaceScripts(javascript, signal, shader, keyboard);
                check(script.Status == "running", "script executes through Quartz SDK: " + script.Status);
                return output;
            };
            check(step().ShaderId == signal.CurrentShaderId, "loud audio selects equalizer");
            check(javascript.ownsShaderMutex(script.Id), "script owns shader mutex");
            check(property(script,"quietShader").StringValue == signal.PreviousShaderId, "captures preceding shader");
            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == signal.PreviousShaderId, "falling RMS restores preceding shader");
            signal.CurrentShaderId = signal.PreviousShaderId;
            signal.PreviousShaderId = "builtin.rainbow_equalizer";
            signal.Audio.Rms = 0.1f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "rising RMS returns to equalizer");
            check(property(script,"quietShader").StringValue == "builtin.neon_triangle", "own transitions preserve remembered shader");
            property(script,"rmsThreshold").NumberValue = 0.2;
            check(step().ShaderId == "builtin.neon_triangle", "threshold changes apply without reload");
            script.Enabled = false; signal.Time += 0.1;
            const auto& disabled = runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard);
            check(!javascript.shaderMutexLocked() && !disabled.ShaderId, "disabling releases mutex and output");
            script.Enabled = true;
            javascript.save();
        }
        {
            JavaScriptRuntime javascript;
            check(javascript.scripts().size() == 1, "script configuration persists");
            auto& competitor = javascript.add();
            competitor.Priority = 1000;
            const auto competitorId = competitor.Id;
            auto& script = javascript.scripts().front();
            signal.CurrentShaderId = signal.PreviousShaderId = "builtin.rainbow_equalizer";
            signal.Time += 0.1; signal.Audio.Rms = 0.01f;
            check(javascript.lockShaderMutex(competitorId), "competing script obtains mutex");
            const auto& busy = runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard);
            check(!busy.ShaderId, "script does not switch while another owner holds mutex");
            javascript.unlockShaderMutex(competitorId); signal.Time += 0.1;
            const auto& output = runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard);
            check(output.ShaderId == "builtin.neon_triangle", "remembered shader survives restart without history");
            check(property(script,"rmsThreshold").NumberValue == 0.2, "threshold persists across restart");
            check(javascript.ownsShaderMutex(script.Id), "script acquires mutex when released");
        }
        // This dedicated directory is created and owned by this test process.
        if (std::filesystem::weakly_canonical(config).parent_path() != temporaryRoot)
            throw std::runtime_error("unexpected test cleanup path");
        std::filesystem::remove_all(config);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
