#include "quartz/client/Model.hpp"

namespace quartz::client
{
    std::vector<ShaderPreset> ShaderPresets = buildShaderPresets();

    const ReactiveKeyBinding* findReactiveKeyBinding(const std::uint16_t key) noexcept
    {
        for (const auto& binding : ReactiveKeyBindings)
            if (binding.Key == key)
                return &binding;
        return nullptr;
    }

    TextEditor::Palette shaderEditorPalette()
    {
        auto palette = TextEditor::GetDarkPalette();
        palette[static_cast<std::size_t>(TextEditor::Color::background)] = IM_COL32(13, 13, 13, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] = IM_COL32(90, 90, 90, 110);
        palette[static_cast<std::size_t>(TextEditor::Color::lineNumber)] = IM_COL32(105, 105, 105, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::currentLineNumber)] = IM_COL32(230, 230, 230, 255);
        palette[static_cast<std::size_t>(TextEditor::Color::matchingBracketBackground)] = IM_COL32(80, 80, 80, 100);
        return palette;
    }

    void configureShaderEditor(TextEditor& editor, const std::string_view source)
    {
        editor.SetPalette(shaderEditorPalette());
        editor.SetLanguage(TextEditor::Language::Glsl());
        editor.SetTabSize(4);
        editor.SetInsertSpacesOnTabs(true);
        editor.SetAutoIndentEnabled(true);
        editor.SetShowLineNumbersEnabled(true);
        editor.SetShowMiniMapEnabled(true);
        editor.SetMiniMapColumns(28);
        editor.SetShowMatchingBrackets(true);
        editor.SetCompletePairedGlyphs(true);
        editor.SetLineFoldingEnabled(true);
        editor.SetWordWrapEnabled(false);
        editor.SetText(source);
    }

    void initializeShaderEditors(ShaderEditorState& state, const std::string_view vertexSource, const std::string_view fragmentSource)
    {
        if (state.Initialized)
            return;
        configureShaderEditor(state.Vertex, vertexSource);
        configureShaderEditor(state.Fragment, fragmentSource);
        state.Initialized = true;
    }



    std::string g_SettingsStatus = "Settings not loaded yet";

    std::filesystem::path settingsPath()
    {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "quartz" / "visualizer.ini";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "quartz" / "visualizer.ini";
        return "quartz_visualizer.ini";
    }

    std::filesystem::path vertexShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.vert";
    }

    std::filesystem::path fragmentShaderPath()
    {
        return settingsPath().parent_path() / "visualizer.frag";
    }

    std::filesystem::path shaderMaterialPath()
    {
        return settingsPath().parent_path() / "visualizer.material.ini";
    }

    void setShaderSource(std::array<char, ShaderSourceCapacity>& destination, const std::string_view source)
    {
        const std::size_t size = std::min(source.size(), destination.size() - 1);
        std::memcpy(destination.data(), source.data(), size);
        destination[size] = '\0';
    }

    bool loadTextFile(const std::filesystem::path& path, std::array<char, ShaderSourceCapacity>& source)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) >= source.size())
            return false;
        file.seekg(0);
        file.read(source.data(), size);
        if (!file)
            return false;
        source[static_cast<std::size_t>(size)] = '\0';
        return true;
    }

    bool saveTextFile(const std::filesystem::path& path, const std::array<char, ShaderSourceCapacity>& source)
    {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(source.data(), static_cast<std::streamsize>(std::strlen(source.data())));
        return static_cast<bool>(file);
    }

    bool replaceAll(std::string& value, const std::string_view from, const std::string_view to)
    {
        if (from.empty())
            return false;
        bool changed = false;
        std::size_t offset = 0;
        while ((offset = value.find(from, offset)) != std::string::npos)
        {
            value.replace(offset, from.size(), to);
            offset += to.size();
            changed = true;
        }
        return changed;
    }

    bool migrateObsoleteShaderSource(std::array<char, ShaderSourceCapacity>& source)
    {
        std::string value(source.data());
        const std::string oldState = std::string("u") + "C" + "trl";
        const std::string oldColor = oldState + "Color";
        const std::string oldEnabled = oldColor + "Enabled";
        bool changed = false;
        changed |= replaceAll(value, oldEnabled, "uCapsLockColorEnabled");
        changed |= replaceAll(value, oldColor, "uCapsLockColor");
        changed |= replaceAll(value, oldState, "uCapsLock");
        changed |= replaceAll(value, "row == 5 && (column == 0 || column == 12)", "row == 3 && column == 0");
        changed |= replaceAll(value, "keyRow == 5 && (keyColumn == 0 || keyColumn == 12)", "keyRow == 3 && keyColumn == 0");
        if (changed)
            setShaderSource(source, value);
        return changed;
    }

    std::filesystem::path shaderLibraryPath()
    {
        return settingsPath().parent_path() / "shaders";
    }

    bool loadTextFileString(const std::filesystem::path& path, std::string& source)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::size_t>(size) >= ShaderSourceCapacity) return false;
        source.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(source.data(), size);
        return static_cast<bool>(file);
    }

    std::optional<std::pair<std::string, std::string>> parseShaderAnnotation(const std::string_view source)
    {
        const std::size_t marker = source.find("@shader");
        if (marker == std::string_view::npos) return std::nullopt;
        const std::size_t end = source.find('\n', marker);
        const std::string line(source.substr(marker, end == std::string_view::npos ? source.size() - marker : end - marker));
        std::smatch match;
        static const std::regex IdQuoted(R"(id\s*=\s*[\"']([^\"']+)[\"'])", std::regex::icase);
        static const std::regex IdBare(R"(id\s*=\s*([A-Za-z0-9_.:/-]+))", std::regex::icase);
        static const std::regex LabelQuoted(R"(label\s*=\s*[\"']([^\"']+)[\"'])", std::regex::icase);
        std::string id, label;
        if (std::regex_search(line, match, IdQuoted)) id = match[1].str();
        else if (std::regex_search(line, match, IdBare)) id = match[1].str();
        if (std::regex_search(line, match, LabelQuoted)) label = match[1].str();
        if (id.empty()) return std::nullopt;
        if (label.empty()) label = id;
        return std::pair{id, label};
    }

    void refreshShaderLibrary()
    {
        std::erase_if(ShaderPresets, [](const ShaderPreset& preset) { return !preset.BuiltIn; });
        std::error_code ec;
        const auto root = shaderLibraryPath();
        std::filesystem::create_directories(root, ec);
        if (ec) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
        {
            if (ec || !entry.is_regular_file()) continue;
            const auto extension = entry.path().extension().string();
            if (extension != ".glsl" && extension != ".frag" && extension != ".fs") continue;
            std::string source;
            if (!loadTextFileString(entry.path(), source)) continue;
            const auto annotation = parseShaderAnnotation(source);
            if (!annotation) continue;
            const auto& [id, label] = *annotation;
            if (std::ranges::any_of(ShaderPresets, [&](const ShaderPreset& preset) { return preset.Id == id; })) continue;
            ShaderPreset preset;
            preset.Name = label;
            preset.FragmentSource = std::move(source);
            preset.Id = id;
            preset.SourcePath = entry.path();
            preset.BuiltIn = false;
            ShaderPresets.emplace_back(std::move(preset));
        }
    }

    const ShaderPreset* findShaderPresetById(const std::string_view id) noexcept
    {
        const auto it = std::ranges::find_if(ShaderPresets, [&](const ShaderPreset& preset) { return preset.Id == id; });
        return it == ShaderPresets.end() ? nullptr : &*it;
    }

    int shaderPresetIndexById(const std::string_view id) noexcept
    {
        for (std::size_t i = 0; i < ShaderPresets.size(); ++i) if (ShaderPresets[i].Id == id) return static_cast<int>(i + 1);
        return 0;
    }

    bool importShaderToLibrary(const std::filesystem::path& sourcePath, std::string& importedId, std::string& error)
    {
        std::string source;
        if (!loadTextFileString(sourcePath, source)) { error = "could not read shader file"; return false; }
        const auto annotation = parseShaderAnnotation(source);
        if (!annotation) { error = "shader has no @shader id=... annotation"; return false; }
        importedId = annotation->first;
        std::error_code ec;
        std::filesystem::create_directories(shaderLibraryPath(), ec);
        if (ec) { error = "could not create shader library: " + ec.message(); return false; }
        const std::filesystem::path destination = shaderLibraryPath() / sourcePath.filename();
        if (sourcePath.lexically_normal() != destination.lexically_normal())
        {
            std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { error = "could not import shader: " + ec.message(); return false; }
        }
        refreshShaderLibrary();
        error.clear();
        return findShaderPresetById(importedId) != nullptr;
    }


    std::string shaderPresetIdByIndex(const int index)
    {
        return index > 0 && index <= static_cast<int>(ShaderPresets.size()) ? ShaderPresets[static_cast<std::size_t>(index - 1)].Id : std::string{};
    }

    void loadShaderSources(std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        setShaderSource(vertexSource, DefaultVertexShaderSource);
        setShaderSource(fragmentSource, ShaderPresets.front().FragmentSource);
        loadTextFile(vertexShaderPath(), vertexSource);
        loadTextFile(fragmentShaderPath(), fragmentSource);
    }

    int detectShaderPreset(const std::string_view source) noexcept
    {
        for (std::size_t i = 0; i < ShaderPresets.size(); ++i)
            if (source == std::string_view(ShaderPresets[i].FragmentSource)) return static_cast<int>(i + 1);
        return 0;
    }

    void saveShaderSources(const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        saveTextFile(vertexShaderPath(), vertexSource);
        saveTextFile(fragmentShaderPath(), fragmentSource);
    }

    std::string serializeSettings(const VisualizerSettings& settings)
    {
        std::ostringstream stream;
        stream << std::boolalpha << std::setprecision(9);
        stream << "Enabled=" << settings.Enabled << '\n';
        stream << "SendFramebuffer=" << settings.SendFramebuffer << '\n';
        stream << "MediaArtworkColor=" << settings.MediaArtworkColor << '\n';
        stream << "ForceFullRow=" << settings.ForceFullRow << '\n';
        stream << "ShowFramebuffer=" << settings.ShowFramebuffer << '\n';
        stream << "ShowAnalysisSpectrum=" << settings.ShowAnalysisSpectrum << '\n';
        stream << "ShowMappedSpectrum=" << settings.ShowMappedSpectrum << '\n';
        stream << "LimitMainLoop=" << settings.LimitMainLoop << '\n';
        stream << "AutoReconnect=" << settings.AutoReconnect << '\n';
        stream << "ShaderRecompileOnChange=" << settings.ShaderRecompileOnChange << '\n';
        stream << "ShaderKeyStateUniforms=" << settings.ShaderKeyStateUniforms << '\n';
        stream << "ShaderCapsLockColorEnabled=" << settings.ShaderCapsLockColorEnabled << '\n';
        stream << "ShaderScrollLockColorEnabled=" << settings.ShaderScrollLockColorEnabled << '\n';
        stream << "ShaderFramebufferWidth=" << settings.ShaderFramebufferWidth << '\n';
        stream << "ShaderFramebufferHeight=" << settings.ShaderFramebufferHeight << '\n';
        stream << "FrameRate=" << settings.FrameRate << '\n';
        stream << "AnalysisBandCount=" << settings.AnalysisBandCount << '\n';
        stream << "BassColumns=" << settings.BassColumns << '\n';
        stream << "BassEndBand=" << settings.BassEndBand << '\n';
        stream << "FullRow=" << settings.FullRow << '\n';
        stream << "OverallGain=" << settings.OverallGain << '\n';
        stream << "AutomaticOverallGain=" << settings.AutomaticOverallGain << '\n';
        stream << "AutoGainBaseline=" << settings.AutoGainBaseline << '\n';
        stream << "AutoGainTargetRms=" << settings.AutoGainTargetRms << '\n';
        stream << "AutoGainAdaptation=" << settings.AutoGainAdaptation << '\n';
        stream << "AutoGainMinCorrection=" << settings.AutoGainMinCorrection << '\n';
        stream << "AutoGainMaxCorrection=" << settings.AutoGainMaxCorrection << '\n';
        stream << "AutoGainSilenceGate=" << settings.AutoGainSilenceGate << '\n';
        stream << "GlobalBrightness=" << settings.GlobalBrightness << '\n';
        stream << "LiveOutputInterpolation=" << settings.LiveOutputInterpolation << '\n';
        stream << "WaveSpeed=" << settings.WaveSpeed << '\n';
        stream << "FeatherRows=" << settings.FeatherRows << '\n';
        stream << "Saturation=" << settings.Saturation << '\n';
        stream << "AttackSpeed=" << settings.AttackSpeed << '\n';
        stream << "ReleaseSpeed=" << settings.ReleaseSpeed << '\n';
        stream << "BassActivationThreshold=" << settings.BassActivationThreshold << '\n';
        stream << "BassMaxBoost=" << settings.BassMaxBoost << '\n';
        stream << "ColorTransitionSpeed=" << settings.ColorTransitionSpeed << '\n';
        stream << "MediaColorBlend=" << settings.MediaColorBlend << '\n';
        stream << "MinFrequency=" << settings.MinFrequency << '\n';
        stream << "MaxFrequency=" << settings.MaxFrequency << '\n';
        stream << "MinDb=" << settings.MinDb << '\n';
        stream << "MaxDb=" << settings.MaxDb << '\n';
        stream << "StatisticsInterval=" << settings.StatisticsInterval << '\n';
        stream << "MediaPollInterval=" << settings.MediaPollInterval << '\n';
        stream << "ShaderEditorZoom=" << settings.ShaderEditorZoom << '\n';
        stream << "ShaderTransitionSeconds=" << settings.ShaderTransitionSeconds << '\n';
        stream << "BaseColorMode=" << settings.BaseColorMode << '\n';
        stream << "ShaderDownsampleMode=" << settings.ShaderDownsampleMode << '\n';
        stream << "ShaderPresetIndex=" << settings.ShaderPresetIndex << '\n';
        stream << "ShaderId=" << settings.ShaderId << '\n';
        stream << "AudioSource=" << settings.AudioSource << '\n';
        stream << "SolidColor=" << settings.SolidColor[0] << ',' << settings.SolidColor[1] << ',' << settings.SolidColor[2] << '\n';
        stream << "ShaderCapsLockColor=" << settings.ShaderCapsLockColor[0] << ',' << settings.ShaderCapsLockColor[1] << ',' << settings.ShaderCapsLockColor[2] << '\n';
        stream << "ShaderScrollLockColor=" << settings.ShaderScrollLockColor[0] << ',' << settings.ShaderScrollLockColor[1] << ',' << settings.ShaderScrollLockColor[2] << '\n';
        stream << "ColumnGain=";
        for (std::size_t i = 0; i < settings.ColumnGain.size(); ++i)
        {
            if (i != 0)
                stream << ',';
            stream << settings.ColumnGain[i];
        }
        stream << '\n';
        return stream.str();
    }

    std::string_view trimSettingValue(std::string_view value) noexcept
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    bool parseBool(const std::string_view value, bool& result)
    {
        if (value == "true" || value == "1") { result = true; return true; }
        if (value == "false" || value == "0") { result = false; return true; }
        return false;
    }

    void loadSettings(VisualizerSettings& settings)
    {
        const auto path = settingsPath();
        std::ifstream file(path);
        if (!file)
        {
            g_SettingsStatus = "No settings file; using defaults";
            return;
        }

        std::size_t loaded = 0;
        std::size_t failed = 0;
        std::string line;
        while (std::getline(file, line))
        {
            std::string_view whole = trimSettingValue(line);
            if (whole.empty() || whole.front() == '#' || whole.front() == ';') continue;
            const std::size_t separator = whole.find('=');
            if (separator == std::string_view::npos) { ++failed; continue; }
            std::string_view key = trimSettingValue(whole.substr(0, separator));
            const std::string_view value = trimSettingValue(whole.substr(separator + 1));
            if (key.size() >= 3 && static_cast<unsigned char>(key[0]) == 0xEF && static_cast<unsigned char>(key[1]) == 0xBB && static_cast<unsigned char>(key[2]) == 0xBF) key.remove_prefix(3);

#define LOAD_BOOL(name) if (key == #name) { if (parseBool(value, settings.name)) ++loaded; else ++failed; continue; }
#define LOAD_NUM(name) if (key == #name) { if (parseNumber(value, settings.name)) ++loaded; else ++failed; continue; }
            LOAD_BOOL(Enabled)
            LOAD_BOOL(SendFramebuffer)
            LOAD_BOOL(MediaArtworkColor)
            LOAD_BOOL(ForceFullRow)
            LOAD_BOOL(ShowFramebuffer)
            LOAD_BOOL(ShowAnalysisSpectrum)
            LOAD_BOOL(ShowMappedSpectrum)
            LOAD_BOOL(LimitMainLoop)
            LOAD_BOOL(AutoReconnect)
            LOAD_BOOL(ShaderRecompileOnChange)
            LOAD_BOOL(ShaderKeyStateUniforms)
            LOAD_BOOL(ShaderCapsLockColorEnabled)
            LOAD_BOOL(ShaderScrollLockColorEnabled)
            LOAD_NUM(ShaderFramebufferWidth)
            LOAD_NUM(ShaderFramebufferHeight)
            LOAD_NUM(FrameRate)
            LOAD_NUM(AnalysisBandCount)
            LOAD_NUM(BassColumns)
            LOAD_NUM(BassEndBand)
            LOAD_NUM(FullRow)
            LOAD_NUM(OverallGain)
            LOAD_BOOL(AutomaticOverallGain)
            LOAD_NUM(AutoGainBaseline)
            LOAD_NUM(AutoGainTargetRms)
            LOAD_NUM(AutoGainAdaptation)
            LOAD_NUM(AutoGainMinCorrection)
            LOAD_NUM(AutoGainMaxCorrection)
            LOAD_NUM(AutoGainSilenceGate)
            LOAD_NUM(GlobalBrightness)
            LOAD_NUM(LiveOutputInterpolation)
            LOAD_NUM(WaveSpeed)
            LOAD_NUM(FeatherRows)
            LOAD_NUM(Saturation)
            LOAD_NUM(AttackSpeed)
            LOAD_NUM(ReleaseSpeed)
            LOAD_NUM(BassActivationThreshold)
            LOAD_NUM(BassMaxBoost)
            LOAD_NUM(ColorTransitionSpeed)
            LOAD_NUM(MediaColorBlend)
            LOAD_NUM(MinFrequency)
            LOAD_NUM(MaxFrequency)
            LOAD_NUM(MinDb)
            LOAD_NUM(MaxDb)
            LOAD_NUM(StatisticsInterval)
            LOAD_NUM(MediaPollInterval)
            LOAD_NUM(ShaderEditorZoom)
            LOAD_NUM(ShaderTransitionSeconds)
            LOAD_NUM(BaseColorMode)
            LOAD_NUM(ShaderDownsampleMode)
#undef LOAD_NUM
#undef LOAD_BOOL
            if (key == "StartMinimized" || key == "StartHidden") { ++loaded; continue; }
            // ShaderPresetIndex is deliberately ignored. The fragment file is authoritative and
            // the preset is detected from its contents after loading, so preset reordering cannot
            // silently replace a user's saved shader.
            if (key == "ShaderPresetIndex") { ++loaded; continue; }
            if (key == "ShaderId") { settings.ShaderId = value; ++loaded; continue; }
            if (key == "AudioSource")
            {
                const std::size_t count = std::min(value.size(), sizeof(settings.AudioSource) - 1);
                std::memcpy(settings.AudioSource, value.data(), count);
                settings.AudioSource[count] = '\0';
                ++loaded;
            }
            else if (key == "SolidColor") { parseFloatArray(value, settings.SolidColor) ? ++loaded : ++failed; }
            else if (key == "ShaderCapsLockColor") { parseFloatArray(value, settings.ShaderCapsLockColor) ? ++loaded : ++failed; }
            else if (key == "ShaderScrollLockColor") { parseFloatArray(value, settings.ShaderScrollLockColor) ? ++loaded : ++failed; }
            else if (key == "ColumnGain") { parseFloatArray(value, settings.ColumnGain) ? ++loaded : ++failed; }
        }
        settings.FrameRate = std::clamp(settings.FrameRate, 30, 500);
        settings.AnalysisBandCount = std::clamp(settings.AnalysisBandCount, 32, static_cast<int>(FFTSize));
        settings.BassColumns = std::clamp(settings.BassColumns, 2, 8);
        settings.BassEndBand = std::clamp(settings.BassEndBand, 0, settings.AnalysisBandCount - 1);
        settings.FullRow = std::clamp(settings.FullRow, 0, static_cast<int>(Rows) - 2);
        settings.BaseColorMode = std::clamp(settings.BaseColorMode, 0, 2);
        settings.ShaderDownsampleMode = std::clamp(settings.ShaderDownsampleMode, 0, 2);
        settings.ShaderFramebufferWidth = std::clamp(settings.ShaderFramebufferWidth, static_cast<int>(Columns), MaxShaderDimension);
        settings.ShaderFramebufferHeight = std::clamp(settings.ShaderFramebufferHeight, static_cast<int>(Rows), MaxShaderDimension);
        settings.ShaderEditorZoom = std::clamp(std::round(settings.ShaderEditorZoom * 10.0f) / 10.0f, 0.60f, 2.50f);
        settings.ShaderTransitionSeconds = std::clamp(settings.ShaderTransitionSeconds, 0.0f, 10.0f);
        settings.GlobalBrightness = std::clamp(settings.GlobalBrightness, 0.0f, 1.0f);
        settings.LiveOutputInterpolation = std::clamp(settings.LiveOutputInterpolation, 0.0f, 1.0f);
        g_SettingsStatus = "Loaded " + std::to_string(loaded) + " settings from " + path.string();
        if (failed != 0) g_SettingsStatus += " (" + std::to_string(failed) + " malformed lines ignored)";
    }

}
