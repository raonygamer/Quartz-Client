int main(int argc, char* argv[])
{
    auto hidden = false;
    if (argc >= 2)
    {
        hidden = strcmp(argv[1], "hidden") == 0;
    }
    VisualizerSettings settings;
    loadSettings(settings);

    glfwSetErrorCallback([](const int error, const char* description) { std::fprintf(stderr, "GLFW error %d: %s\n", error, description); });
    if (!glfwInit())
        return EXIT_FAILURE;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Quartz", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwSetWindowCloseCallback(window, [](GLFWwindow* target)
    {
        glfwSetWindowShouldClose(target, GLFW_FALSE);
        glfwHideWindow(target);
    });

    glfwMakeContextCurrent(window);
#ifdef GLFW_LOCK_KEY_MODS
    glfwSetInputMode(window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
#endif
    glfwSwapInterval(0);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)))
    {
        std::fprintf(stderr, "Failed to initialize OpenGL loader\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    applyDarkTheme();
    initializeShaderEditorFonts();

    constexpr const char* GLSLVersion = "#version 330";
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true))
    {
        std::fprintf(stderr, "Failed to initialize ImGui GLFW backend\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (!ImGui_ImplOpenGL3_Init(GLSLVersion))
    {
        std::fprintf(stderr, "Failed to initialize ImGui OpenGL backend\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    RawUSB usb;
    AudioSpectrum audio;
    MediaColorProvider mediaColor;
    ShaderFramebuffer shaderFramebuffer;
    ShaderTransitionState shaderTransition;
    ShaderEditorState shaderEditor;
    ViewPage page = ViewPage::Main;
    std::array<char, ShaderSourceCapacity> vertexShaderSource{};
    std::array<char, ShaderSourceCapacity> fragmentShaderSource{};
    std::array<char, ShaderPathCapacity> vertexLoadPath{};
    std::array<char, ShaderPathCapacity> fragmentLoadPath{};
    refreshShaderLibrary();
    loadShaderSources(vertexShaderSource, fragmentShaderSource);
    if (migrateObsoleteShaderSource(fragmentShaderSource))
        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
    const auto defaultVertexPath = vertexShaderPath().string();
    const auto defaultFragmentPath = fragmentShaderPath().string();
    std::snprintf(vertexLoadPath.data(), vertexLoadPath.size(), "%s", defaultVertexPath.c_str());
    std::snprintf(fragmentLoadPath.data(), fragmentLoadPath.size(), "%s", defaultFragmentPath.c_str());
    if (std::string_view(fragmentShaderSource.data()) == LegacyDefaultFragmentShaderSource || std::string_view(fragmentShaderSource.data()) == DefaultFragmentShaderSource)
    {
        setShaderSource(fragmentShaderSource, ShaderPresets.front().FragmentSource);
        saveTextFile(fragmentShaderPath(), fragmentShaderSource);
    }
    settings.ShaderPresetIndex = detectShaderPreset(fragmentShaderSource.data());
    if (settings.ShaderPresetIndex > 0) settings.ShaderId = shaderPresetIdByIndex(settings.ShaderPresetIndex);
    initializeShaderEditors(shaderEditor, vertexShaderSource.data(), fragmentShaderSource.data());
    SharedDeviceState deviceState;
    std::array<float, FFTSize> analysisBands{};
    std::array<float, Columns> mappedBands{};
    std::array<float, Columns> smoothedBands{};
    std::array<Color32, MatrixSize> framebuffer{};
    std::optional<Color32> visualizerColor;
    float mediaColorAmount = 0.0f;
    std::uint64_t sentFrames = 0;
    std::uint64_t droppedFrames = 0;
    AppCpuMeter appCpuMeter;
    float appCpuUsage = 0.0f;
    EvdevKeyboard keyboardInput;
    ReactiveKeyState reactiveKeys;
    RuntimeInputAnalytics inputAnalytics;
    RuntimeRGBAnalytics rgbAnalytics;
    RuntimeBindingEngine runtimeBindings;
    RuntimeTelemetry runtimeTelemetry;
    AutoGainState autoGain;
    autoGain.reset(settings);
    AudioLevelSnapshot audioLevel{};

    loadShaderMaterialValueCache();
    shaderFramebuffer.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight);
    compileShaders(shaderFramebuffer, shaderEditor, vertexShaderSource, fragmentShaderSource);

    usb.setPacketObserver([&](const bool tx, const PacketHeader& packet, const std::span<const std::byte> bytes)
    {
        runtimeTelemetry.packet(glfwGetTime(), tx, packet, bytes);
    });

    usb.setPacketHandler([&](const PacketHeader& packet)
    {
        if (!packet.isDeviceToHost())
            return;
        std::lock_guard lock(deviceState.Mutex);
        ++deviceState.ReceivedPackets;
        if (packet.Type == PacketType::PerformanceResponse)
        {
            if (const auto* payload = packet.getPayload<PerformancePayload>())
            {
                deviceState.Performance = {
                    payload->CoreClock,
                    payload->BeginScanTicks,
                    payload->ScanTicks,
                    payload->EndScanTicks,
                    payload->StateUpdateTicks,
                    payload->HIDTicks,
                    payload->RGBTicks,
                    payload->AverageScanPeriodTicks,
                    payload->RGBSlotMaxTicks
                };
                deviceState.HasPerformance = true;
            }
        }
        else if (packet.Type == PacketType::MatrixTimingProbeResult)
        {
            if (const auto* payload = packet.getPayload<MatrixTimingProbeResult<ActiveProbeRows>>())
            {
                deviceState.TimingProbe = *payload;
                deviceState.HasTimingProbe = true;
            }
        }
    });

    try
    {
        if (!usb.initialize())
            throw std::runtime_error(libusb_error_name(usb.lastError()));
        usb.connect();
        audio.start(settings.AudioSource);
        mediaColor.start();
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "Quartz initialization failed: %s\n", exception.what());
    }

    const double startTime = glfwGetTime();
    keyboardInput.start(startTime);
    runtimeTelemetry.event(startTime, "Application", "Quartz runtime studio started");
    double nextVisualizerFrame = startTime;
    double nextStatisticsRequest = startTime;
    double nextReconnectAttempt = startTime;
    double nextSettingsSave = startTime + 0.50;
    std::string savedSettings = serializeSettings(settings);
    std::uint64_t savedMaterialRevision = shaderFramebuffer.materialRevision();
    bool lastUSBConnected = usb.isConnected();
    std::string lastMediaTitle = mediaColor.mediaTitle();
    bool lastMediaPlaying = mediaColor.playing();
    bool lastCapsLock = reactiveKeys.CapsLockActive;
    bool lastScrollLock = reactiveKeys.ScrollLockActive;
    if (hidden)
    {
        glfwHideWindow(window);
    }
    while (!glfwWindowShouldClose(window))
    {
        if (keyboardInput.consumeRestoreRequest()) restoreWindow(window);
        const double currentFrame = glfwGetTime();
        appCpuUsage = appCpuMeter.update(currentFrame);
        runtimeBindings.updateRates(usb.stats(), currentFrame);
        glfwPollEvents();
        runtimeBindings.pollProfileHotkeys(window);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        reactiveKeys = keyboardInput.snapshot();
        for (std::size_t i = 0; i < MatrixSize; ++i)
        {
            const bool down = reactiveKeys.Down[i] > 0.5f;
            const bool wasDown = inputAnalytics.Previous[i] > 0.5f;
            if (down != wasDown)
            {
                const std::size_t row = i / Columns, column = i % Columns;
                runtimeTelemetry.event(currentFrame, "Input", "r" + std::to_string(row) + " c" + std::to_string(column) + (down ? " down" : " up"));
            }
        }
        inputAnalytics.update(reactiveKeys, currentFrame);
        const bool scrollLockActive = reactiveKeys.ScrollLockActive;
        const bool capsLockActive = reactiveKeys.CapsLockActive;
        if (capsLockActive != lastCapsLock)
        {
            runtimeTelemetry.event(currentFrame, "Input", std::string("Caps Lock ") + (capsLockActive ? "on" : "off"));
            lastCapsLock = capsLockActive;
        }
        if (scrollLockActive != lastScrollLock)
        {
            runtimeTelemetry.event(currentFrame, "Input", std::string("Scroll Lock ") + (scrollLockActive ? "on" : "off"));
            lastScrollLock = scrollLockActive;
        }

        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderPresetIndex = std::clamp(settings.ShaderPresetIndex, 0, static_cast<int>(ShaderPresets.size()));
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.ShaderTransitionSeconds = std::clamp(settings.ShaderTransitionSeconds, 0.0f, 10.0f);
        settings.AutoGainBaseline = std::clamp(settings.AutoGainBaseline, 0.05f, 20.0f);
        settings.AutoGainTargetRms = std::clamp(settings.AutoGainTargetRms, 0.005f, 0.75f);
        settings.AutoGainAdaptation = std::clamp(settings.AutoGainAdaptation, 0.01f, 5.0f);
        settings.AutoGainMinCorrection = std::clamp(settings.AutoGainMinCorrection, 0.05f, 10.0f);
        settings.AutoGainMaxCorrection = std::clamp(settings.AutoGainMaxCorrection, settings.AutoGainMinCorrection, 20.0f);
        settings.AutoGainSilenceGate = std::clamp(settings.AutoGainSilenceGate, 0.0f, 0.25f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.LiveOutputInterpolation = std::clamp(settings.LiveOutputInterpolation, 0.0f, 1.0f);
        settings.FeatherRows = std::max(settings.FeatherRows, 0.01f);
        settings.MaxFrequency = std::max(settings.MaxFrequency, settings.MinFrequency + 1.0f);
        settings.MaxDb = std::max(settings.MaxDb, settings.MinDb + 0.1f);
        mediaColor.setPollInterval(settings.MediaPollInterval);

        const bool usbConnectedNow = usb.isConnected();
        if (usbConnectedNow != lastUSBConnected)
        {
            runtimeTelemetry.event(currentFrame, "USB", usbConnectedNow ? "Device connected" : "Device disconnected");
            lastUSBConnected = usbConnectedNow;
        }
        const std::string mediaTitleNow = mediaColor.mediaTitle();
        const bool mediaPlayingNow = mediaColor.playing();
        if (mediaTitleNow != lastMediaTitle || mediaPlayingNow != lastMediaPlaying)
        {
            runtimeTelemetry.event(currentFrame, "Media", mediaPlayingNow ? "Playing: " + mediaTitleNow : "Paused/stopped: " + mediaTitleNow);
            lastMediaTitle = mediaTitleNow;
            lastMediaPlaying = mediaPlayingNow;
        }

        if (settings.AutoReconnect && !usb.isConnected() && currentFrame >= nextReconnectAttempt)
        {
            usb.connect();
            nextReconnectAttempt = currentFrame + 1.0;
        }

        const double visualizerFrameTime = 1.0 / std::max(1, settings.FrameRate);
        if (settings.Enabled && currentFrame >= nextVisualizerFrame)
        {
            audioLevel = audio.levelSnapshot();
            autoGain.update(audioLevel, settings, static_cast<float>(visualizerFrameTime));
            audio.getBands(std::span(analysisBands).first(settings.AnalysisBandCount), settings.MinFrequency, settings.MaxFrequency, settings.MinDb, settings.MaxDb);
            mapSpectrumToColumns(std::span<const float>(analysisBands).first(settings.AnalysisBandCount), mappedBands, settings, autoGain.EffectiveGain);
            smoothBands(mappedBands, smoothedBands, settings, static_cast<float>(visualizerFrameTime));

            const float colorAlpha = 1.0f - std::exp(-settings.ColorTransitionSpeed * static_cast<float>(visualizerFrameTime));
            const auto mediaTarget = settings.MediaArtworkColor ? mediaColor.targetColor() : std::nullopt;
            if (mediaTarget)
                visualizerColor = visualizerColor ? lerpColor(*visualizerColor, *mediaTarget, colorAlpha) : mediaTarget;
            const float targetColorAmount = mediaTarget ? 1.0f : 0.0f;
            mediaColorAmount += (targetColorAmount - mediaColorAmount) * colorAlpha;
            if (std::abs(targetColorAmount - mediaColorAmount) < 0.001f)
                mediaColorAmount = targetColorAmount;

            PerformanceSnapshot runtimePerformance{};
            bool runtimeHasPerformance = false;
            {
                std::lock_guard lock(deviceState.Mutex);
                runtimePerformance = deviceState.Performance;
                runtimeHasPerformance = deviceState.HasPerformance;
            }
            RuntimeSignalContext runtimeContext;
            runtimeContext.Time = currentFrame;
            runtimeContext.DeltaTime = static_cast<float>(visualizerFrameTime);
            runtimeContext.Audio = audioLevel;
            runtimeContext.MappedBands = &mappedBands;
            runtimeContext.SmoothedBands = &smoothedBands;
            runtimeContext.MediaColor = visualizerColor;
            runtimeContext.MediaAmount = mediaColorAmount;
            runtimeContext.MediaPlaying = mediaColor.playing();
            runtimeContext.MediaTitle = mediaColor.mediaTitle();
            runtimeContext.Keys = reactiveKeys;
            runtimeContext.Performance = runtimePerformance;
            runtimeContext.HasPerformance = runtimeHasPerformance;
            runtimeContext.AppCpu = appCpuUsage;
            runtimeContext.USBConnected = usb.isConnected();
            runtimeContext.USB = usb.stats();
            runtimeContext.USBRates = runtimeBindings.usbRates();
            runtimeContext.Framebuffer = &framebuffer;
            runtimeContext.EffectiveGain = autoGain.EffectiveGain;
            runtimeContext.GainCorrection = autoGain.Correction;
            runtimeContext.CurrentShaderPreset = settings.ShaderPresetIndex;
            runtimeContext.CurrentShaderId = settings.ShaderId;
            runtimeContext.PreviousShaderId = runtimeBindings.previousShaderId();
            runtimeContext.ShaderTransitionActive = shaderTransition.Active;
            runtimeContext.ShaderTransitionProgress = shaderTransition.Active ? std::clamp(static_cast<float>((currentFrame - shaderTransition.StartedAt) / std::max(shaderTransition.Duration, 0.0001f)), 0.0f, 1.0f) : 1.0f;
            runtimeContext.BaseColorMode = settings.BaseColorMode;
            runtimeContext.GlobalBrightness = settings.GlobalBrightness;
            runtimeContext.SendFramebuffer = settings.SendFramebuffer;
            runtimeContext.ShaderFramebufferWidth = settings.ShaderFramebufferWidth;
            runtimeContext.ShaderFramebufferHeight = settings.ShaderFramebufferHeight;
            runtimeBindings.update(runtimeContext, shaderFramebuffer);
            const RuntimeControlOutput controlOutput = runtimeBindings.evaluateControls(shaderFramebuffer);
            if (controlOutput.ShaderId && *controlOutput.ShaderId != settings.ShaderId)
            {
                if (switchShaderId(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, *controlOutput.ShaderId, currentFrame, controlOutput.ShaderTransitionSeconds, false)) runtimeBindings.applyMaterialValues(shaderFramebuffer);
            }
            else if (controlOutput.ShaderPresetIndex && *controlOutput.ShaderPresetIndex != settings.ShaderPresetIndex)
            {
                if (switchShaderPreset(shaderFramebuffer, shaderTransition, shaderEditor, vertexShaderSource, fragmentShaderSource, settings, *controlOutput.ShaderPresetIndex, currentFrame, controlOutput.ShaderTransitionSeconds, false)) runtimeBindings.applyMaterialValues(shaderFramebuffer);
            }

            const int effectiveBaseColorMode = controlOutput.BaseColorMode.value_or(settings.BaseColorMode);
            const float effectiveBrightness = controlOutput.GlobalBrightness.value_or(settings.GlobalBrightness);
            const bool effectiveSendFramebuffer = controlOutput.SendFramebuffer.value_or(settings.SendFramebuffer);
            if (effectiveBaseColorMode == 2)
            {
                if (!shaderFramebuffer.render(currentFrame, smoothedBands, settings, visualizerColor, mediaColorAmount, reactiveKeys, framebuffer))
                    framebuffer.fill({0, 0, 0});
                if (shaderTransition.Active)
                {
                    const float duration = std::max(shaderTransition.Duration, 0.0001f);
                    const float linear = std::clamp(static_cast<float>((currentFrame - shaderTransition.StartedAt) / duration), 0.0f, 1.0f);
                    const float blend = linear * linear * (3.0f - 2.0f * linear);
                    if (shaderTransition.Previous.render(currentFrame, smoothedBands, settings, visualizerColor, mediaColorAmount, reactiveKeys, shaderTransition.Frame))
                        for (std::size_t i = 0; i < MatrixSize; ++i) framebuffer[i] = lerpColorLinear(shaderTransition.Frame[i], framebuffer[i], blend);
                    if (linear >= 1.0f) shaderTransition.cancel();
                }
            }
            else
                renderAudioRGB(framebuffer, smoothedBands, settings, visualizerColor, mediaColorAmount, static_cast<float>(currentFrame * settings.WaveSpeed));
            applyGlobalBrightness(framebuffer, effectiveBrightness);
            rgbAnalytics.update(framebuffer, currentFrame);
            if (effectiveSendFramebuffer && usb.isConnected())
            {
                if (sendFramebuffer(usb, framebuffer))
                    ++sentFrames;
                else
                    ++droppedFrames;
            }

            nextVisualizerFrame += visualizerFrameTime;
            if (currentFrame - nextVisualizerFrame > visualizerFrameTime)
                nextVisualizerFrame = currentFrame + visualizerFrameTime;
        }

        if (usb.isConnected() && currentFrame >= nextStatisticsRequest)
        {
            usb.send(makePacket(PacketType::PerformanceRequest));
            nextStatisticsRequest = currentFrame + std::max(0.05f, settings.StatisticsInterval);
        }

        drawUi(usb, audio, mediaColor, keyboardInput, shaderFramebuffer, shaderTransition, shaderEditor, page, vertexShaderSource, fragmentShaderSource, vertexLoadPath, fragmentLoadPath, settings, analysisBands, mappedBands, smoothedBands, framebuffer, deviceState, runtimeBindings, runtimeTelemetry, autoGain, audioLevel, reactiveKeys, inputAnalytics, rgbAnalytics, sentFrames, droppedFrames, appCpuUsage, scrollLockActive, capsLockActive);
        if (currentFrame >= nextSettingsSave)
        {
            const std::string currentSettings = serializeSettings(settings);
            if (currentSettings != savedSettings && saveSettings(settings))
                savedSettings = currentSettings;
            if (shaderFramebuffer.materialRevision() != savedMaterialRevision && shaderFramebuffer.saveMaterialValues())
                savedMaterialRevision = shaderFramebuffer.materialRevision();
            runtimeBindings.saveIfChanged();
            nextSettingsSave = currentFrame + 0.50;
        }
        ImGui::Render();

        int width;
        int height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (settings.LimitMainLoop)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
    }

    saveSettings(settings);
    saveShaderSources(vertexShaderSource, fragmentShaderSource);
    shaderFramebuffer.saveMaterialValues();
    runtimeBindings.save();
    keyboardInput.stop();
    shaderTransition.cancel();
    shaderFramebuffer.shutdown();
    usb.shutdown();
    mediaColor.stop();
    audio.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
