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
    description: "RMS must stay below this level for the release time, or at/above it for the attack time."
});
const attack = Property.Float32("attackSeconds", {
    label: "Attack time (seconds)", group: "Audio switching",
    default: 0.2, min: 0, max: 30, step: 0.01,
    description: "Time RMS must stay at or above the threshold before switching to the equalizer. Zero switches immediately."
});
const release = Property.Float32("releaseSeconds", {
    label: "Release time (seconds)", group: "Audio switching",
    default: 0.5, min: 0, max: 30, step: 0.01,
    description: "Time RMS must stay below the threshold before restoring the quiet shader. Zero switches immediately."
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

let activeShader = "";
let pendingQuiet = null;
let pendingSince = 0;
let pendingThreshold = threshold.value;
let wasOwner = false;

System.on("update", () => {
    Script.state.rms = Audio.rms;
    Script.state.threshold = threshold.value;
    if (!ShaderMutex.owned && !ShaderMutex.lock()) {
        wasOwner = false;
        pendingQuiet = null;
        Script.state.ownsMutex = false;
        Script.state.remainingSeconds = 0;
        Script.state.status = "Waiting for shader mutex";
        return;
    }
    Script.state.ownsMutex = true;
    if (!wasOwner) activeShader = Runtime.currentShader();
    wasOwner = true;
    if (!quietShader.value) quietShader.value = previousShader();

    const quiet = Audio.rms < threshold.value;
    const target = quiet ? quietShader.value : equalizer.value;
    if (!target || quietShader.value === equalizer.value) {
        Runtime.clearShader();
        pendingQuiet = null;
        Script.state.remainingSeconds = 0;
        Script.state.status = "Choose a different Quiet shader in the script settings";
        return;
    }

    // A crossing starts a new uninterrupted hold; brief bursts never accumulate.
    // Use elapsed runtime time, independent of the script's update frequency.
    const now = System.time;
    if (quiet !== pendingQuiet || threshold.value !== pendingThreshold || now < pendingSince) {
        pendingQuiet = quiet;
        pendingThreshold = threshold.value;
        pendingSince = now;
    }
    const delay = quiet ? release.value : attack.value;
    const remaining = Math.max(0, delay - (now - pendingSince));
    if (remaining === 0) activeShader = target;

    // Hold the current shader, including on enable/reload, until the timer passes.
    if (activeShader) Runtime.setShader(activeShader, transition.value);
    else Runtime.clearShader();
    const waiting = activeShader !== target;
    Script.state.remainingSeconds = waiting ? remaining : 0;
    Script.state.status = waiting
        ? (quiet ? "Waiting for release" : "Waiting for attack")
        : (quiet ? "Quiet: previous shader" : "Audio: equalizer");
    Script.state.targetShader = activeShader;
});

System.on("dispose", () => {
    Runtime.clearShader();
    ShaderMutex.unlock();
});
