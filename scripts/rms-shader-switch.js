import { Audio, Property, Runtime, Script, ShaderMutex, System } from "@quartz/client";

Script.configure({
    id: "quartz.rms-shader-switch",
    name: "RMS shader switch",
    updateRate: 60,
    timeout: 8,
    priority: 100
});

const threshold = Property.Float32("rmsThreshold", {
    label: "RMS threshold", group: "Audio switching",
    default: 0.02, min: 0, max: 1, step: 0.001,
    description: "Below this level, restore the previous shader. At or above it, use the equalizer."
});
const equalizer = Property.Shader("equalizerShader", {
    label: "Equalizer shader", group: "Audio switching",
    default: "builtin.rainbow_equalizer"
});

function previousShader() {
    const current = Runtime.currentShader();
    const previous = Runtime.previousShader();
    if (current && current !== equalizer.value) return current;
    if (previous && previous !== equalizer.value) return previous;
    return "";
}

// Properties persist in Quartz storage, including across application restarts.
// Capture once so our own alternating shader changes cannot overwrite this.
const quietShader = Property.Shader("quietShader", {
    label: "Quiet shader (previous)", group: "Audio switching",
    default: previousShader(),
    description: "Initially the shader used before the equalizer. Choose another here to change it."
});
const transition = Property.Float32("transitionSeconds", {
    label: "Transition seconds", group: "Audio switching",
    default: 0.35, min: 0, max: 5, step: 0.05
});

System.on("update", () => {
    Script.state.rms = Audio.rms;
    Script.state.threshold = threshold.value;
    if (!ShaderMutex.owned && !ShaderMutex.lock()) {
        Script.state.status = "Waiting for shader mutex";
        return;
    }
    Script.state.ownsMutex = true;
    if (!quietShader.value) quietShader.value = previousShader();

    const quiet = Audio.rms < threshold.value;
    const target = quiet ? quietShader.value : equalizer.value;
    if (!target || quietShader.value === equalizer.value) {
        Runtime.clearShader();
        Script.state.status = "Choose a different Quiet shader in the script settings";
        return;
    }
    Runtime.setShader(target, transition.value);
    Script.state.status = quiet ? "Quiet: previous shader" : "Audio: equalizer";
    Script.state.targetShader = target;
});

System.on("dispose", () => {
    Runtime.clearShader();
    ShaderMutex.unlock();
});
