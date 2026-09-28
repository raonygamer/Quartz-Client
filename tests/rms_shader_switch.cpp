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
            const auto step = [&](double seconds = 0.125) -> const RuntimeControlOutput& {
                signal.Time += seconds;
                const auto& output = runtimeEvaluateWorkspaceScripts(javascript, signal, shader, keyboard);
                check(script.Status == "running", "script executes through Quartz SDK: " + script.Status);
                if (output.ShaderId && *output.ShaderId != signal.CurrentShaderId) {
                    signal.PreviousShaderId = signal.CurrentShaderId;
                    signal.CurrentShaderId = *output.ShaderId;
                }
                return output;
            };
            check(step().ShaderId == signal.CurrentShaderId, "loud audio selects equalizer");
            check(javascript.ownsShaderMutex(script.Id), "script owns shader mutex");
            check(property(script,"quietShader").StringValue == signal.PreviousShaderId, "captures preceding shader");
            check(std::abs(property(script,"attackSeconds").NumberValue - 0.2) < 0.00001, "attack defaults to 0.2 seconds");
            check(std::abs(property(script,"releaseSeconds").NumberValue - 0.5) < 0.00001, "release defaults to 0.5 seconds");
            property(script,"attackSeconds").NumberValue = 0.5;
            property(script,"releaseSeconds").NumberValue = 1.0;

            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "falling RMS starts release without switching");
            check(step(0.75).ShaderId == "builtin.rainbow_equalizer", "release holds equalizer before deadline");
            signal.Audio.Rms = 0.1f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "brief recovery cancels release");
            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "next quiet period starts a fresh release");
            check(step(0.875).ShaderId == "builtin.rainbow_equalizer", "separate quiet periods do not accumulate");
            check(step().ShaderId == "builtin.neon_triangle", "continuous quiet switches at release deadline");

            signal.Audio.Rms = 0.1f;
            check(step().ShaderId == "builtin.neon_triangle", "rising RMS starts attack without switching");
            check(step(0.375).ShaderId == "builtin.neon_triangle", "attack holds quiet shader before deadline");
            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.neon_triangle", "brief dip cancels attack");
            signal.Audio.Rms = 0.1f;
            check(step().ShaderId == "builtin.neon_triangle", "next loud period starts a fresh attack");
            check(step(0.5).ShaderId == "builtin.rainbow_equalizer", "continuous loud audio switches at attack deadline");
            check(property(script,"quietShader").StringValue == "builtin.neon_triangle", "own transitions preserve remembered shader");

            property(script,"rmsThreshold").NumberValue = 0.2;
            check(step().ShaderId == "builtin.rainbow_equalizer", "threshold edit begins a release wait");
            check(step(0.5).ShaderId == "builtin.rainbow_equalizer", "threshold edit respects release time");
            property(script,"rmsThreshold").NumberValue = 0.3;
            check(step().ShaderId == "builtin.rainbow_equalizer", "another threshold edit resets pending wait");
            check(step(0.875).ShaderId == "builtin.rainbow_equalizer", "old threshold time is not reused");
            check(step().ShaderId == "builtin.neon_triangle", "new threshold applies without reload");

            // An exactly representable threshold checks the equality boundary.
            property(script,"rmsThreshold").NumberValue = 0.25;
            signal.Audio.Rms = 0.25f;
            check(step().ShaderId == "builtin.neon_triangle", "RMS equal to threshold starts attack");
            check(step(0.5).ShaderId == "builtin.rainbow_equalizer", "RMS equal to threshold completes attack");
            property(script,"releaseSeconds").NumberValue = 0;
            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.neon_triangle", "zero release switches immediately");
            property(script,"attackSeconds").NumberValue = 0;
            signal.Audio.Rms = 0.5f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "zero attack switches immediately");

            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.neon_triangle", "quiet shader remains remembered");
            property(script,"attackSeconds").NumberValue = 1.0;
            signal.Audio.Rms = 0.5f;
            check(step().ShaderId == "builtin.neon_triangle", "long attack starts waiting");
            check(step(0.25).ShaderId == "builtin.neon_triangle", "long attack still waits");
            property(script,"attackSeconds").NumberValue = 0.25;
            check(step().ShaderId == "builtin.rainbow_equalizer", "editing attack adjusts current wait live");
            property(script,"releaseSeconds").NumberValue = 0.75;
            signal.Audio.Rms = 0.001f;
            check(step().ShaderId == "builtin.rainbow_equalizer", "release pending before disable");
            check(step(0.5).ShaderId == "builtin.rainbow_equalizer", "release has not completed before disable");
            script.Enabled = false; signal.Time += 0.125;
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
            signal.Time += 0.125; signal.Audio.Rms = 0.01f;
            check(javascript.lockShaderMutex(competitorId), "competing script obtains mutex");
            const auto& busy = runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard);
            check(!busy.ShaderId, "script does not switch while another owner holds mutex");
            javascript.unlockShaderMutex(competitorId); signal.Time += 1.0;
            check(runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId == "builtin.rainbow_equalizer", "startup keeps current shader and excludes time without mutex");
            signal.Time += 0.5;
            check(runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId == "builtin.rainbow_equalizer", "startup release waits the full duration");
            check(javascript.lockShaderMutex(competitorId), "competitor preempts pending release");
            signal.Time += 0.125;
            check(!runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId, "preempted script stops shader output");
            javascript.unlockShaderMutex(competitorId); signal.Time += 1.0;
            check(runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId == "builtin.rainbow_equalizer", "reacquiring mutex resets release timer");
            signal.Time += 0.5;
            check(runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId == "builtin.rainbow_equalizer", "preemption discards the previous partial release");
            signal.Time += 0.25;
            check(runtimeEvaluateWorkspaceScripts(javascript,signal,shader,keyboard).ShaderId == "builtin.neon_triangle", "remembered shader survives restart and switches after full release");
            check(property(script,"rmsThreshold").NumberValue == 0.25, "threshold persists across restart");
            check(property(script,"attackSeconds").NumberValue == 0.25 && property(script,"releaseSeconds").NumberValue == 0.75, "attack and release persist across restart");
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
