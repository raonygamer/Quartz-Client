import { Audio, Events, Property, Runtime, Script, ShaderMutex, System } from "@quartz/client";

Script.configure({
    id: "rainbow-wave-mpris",
    name: "Rainbow Wave MPRIS",
    priority: 10
});

const IDLE_SHADER = "rainbow_wave";
const MUSIC_SHADER = "builtin.rainbow_equalizer";

const rmsThreshold = Property.Float32("rms-threshold", {
    label: "RMS Threshold",
    group: "Audio detection",
    default: 0.01,
    min: 0.0,
    max: 1.0,
    step: 0.001
});

const rmsAttackSampleTime = Property.Float32("rms-attack-sample-time", {
    label: "RMS Attack Sample Time",
    group: "Audio detection",
    description: "Seconds RMS must remain above the threshold before activating the visualizer.",
    default: 0.2,
    min: 0.0,
    max: 5.0,
    step: 0.05
});

const rmsReleaseSampleTime = Property.Float32("rms-release-sample-time", {
    label: "RMS Release Sample Time",
    group: "Audio detection",
    description: "Seconds RMS must remain below the threshold before restoring the previous shader.",
    default: 2.0,
    min: 0.0,
    max: 10.0,
    step: 0.05
});

let mediaPlaying = false;
let rmsDetected = false;
let rmsAttackElapsed = 0.0;
let rmsReleaseElapsed = 0.0;
let shaderSessionActive = false;
let mutexBusyLogged = false;
let previousShader: string | undefined;
let releaseShader: string | undefined;

function resetRmsTimers(): void {
    rmsAttackElapsed = 0.0;
    rmsReleaseElapsed = 0.0;
}

function sampleRms(deltaTime: number): void {
    const elapsed = Math.max(0.0, deltaTime);
    if (Audio.rms >= rmsThreshold.value) {
        rmsReleaseElapsed = 0.0;

        if (!rmsDetected) {
            rmsAttackElapsed += elapsed;
            if (rmsAttackElapsed >= rmsAttackSampleTime.value) {
                rmsDetected = true;
                rmsAttackElapsed = 0.0;
                console.info(`RMS: audio detected at ${Audio.rms.toFixed(4)}`);
            }
        }
    } else {
        rmsAttackElapsed = 0.0;

        if (rmsDetected) {
            rmsReleaseElapsed += elapsed;
            if (rmsReleaseElapsed >= rmsReleaseSampleTime.value) {
                rmsDetected = false;
                rmsReleaseElapsed = 0.0;
                console.info(`RMS: audio released at ${Audio.rms.toFixed(4)}`);
            }
        }
    }
}

function acquireShaderMutex(): boolean {
    if (ShaderMutex.owned)
        return true;

    if (!ShaderMutex.lock()) {
        if (!mutexBusyLogged) {
            console.info("MPRIS: shader mutex is owned by another script; waiting");
            mutexBusyLogged = true;
        }
        return false;
    }

    if (mutexBusyLogged)
        console.info("MPRIS: shader mutex acquired; resuming pending shader change");
    mutexBusyLogged = false;
    return true;
}

function clearShaderSession(): void {
    Runtime.clearShader();
    delete Script.storage.previousShader;
    previousShader = undefined;
    releaseShader = undefined;
    shaderSessionActive = false;

    if (ShaderMutex.owned)
        ShaderMutex.unlock();
}

function startMusic(): void {
    // Playback may resume while restoring the idle shader. Keep the original
    // shader as the eventual restore target and cancel that pending release.
    if (releaseShader) {
        previousShader = releaseShader;
        releaseShader = undefined;
    }

    if (!shaderSessionActive && Runtime.currentShader() !== IDLE_SHADER)
        return;

    if (!acquireShaderMutex())
        return;

    if (!shaderSessionActive) {
        // Recheck after acquiring so a delayed request never saves or replaces
        // a shader that became active while this script was waiting.
        if (Runtime.currentShader() !== IDLE_SHADER) {
            ShaderMutex.unlock();
            return;
        }

        previousShader = Runtime.currentShader();
        Script.storage.previousShader = previousShader;
        shaderSessionActive = true;
    }

    if (!Runtime.setShader(MUSIC_SHADER)) {
        clearShaderSession();
    }
}

function stopMusic(): void {
    if (!shaderSessionActive)
        return;

    if (!acquireShaderMutex())
        return;

    if (!releaseShader) {
        const restoreTarget = previousShader;

        if (!restoreTarget || !Runtime.setShader(restoreTarget)) {
            clearShaderSession();
            return;
        }

        releaseShader = restoreTarget;
        previousShader = undefined;
    }

    // Keep ownership until the restored shader reaches the application. If a
    // higher-priority script preempts us, the next update reacquires the mutex
    // and the retained output makes this comparison eventually succeed.
    if (Runtime.currentShader() !== releaseShader)
        return;

    clearShaderSession();
    console.info("MPRIS: previous shader restored; shader mutex released");
}

Events.on("media.playback_changed", event => {
    console.info(`MPRIS: ${event.previous} -> ${event.playing}, "${event.title}"`);

    mediaPlaying = event.playing;
    resetRmsTimers();

    if (mediaPlaying) {
        // MPRIS playback is authoritative and activates the visualizer without
        // waiting for the RMS attack window. Silence cannot release it while
        // the player still reports Playing=true.
        rmsDetected = true;
        startMusic();
    }
});

System.on("update", event => {
    if (mediaPlaying) {
        rmsDetected = true;
        resetRmsTimers();
        startMusic();
        return;
    }

    sampleRms(event.deltaTime);

    if (rmsDetected)
        startMusic();
    else
        stopMusic();
});

System.on("dispose", () => {
    mediaPlaying = false;
    rmsDetected = false;
    resetRmsTimers();
    clearShaderSession();
});
