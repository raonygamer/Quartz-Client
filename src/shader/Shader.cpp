#include "quartz/client/Model.hpp"

namespace quartz::client
{
    bool saveSettings(const VisualizerSettings& settings)
    {
        const auto path = settingsPath();
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
        {
            std::ofstream file(temporaryPath, std::ios::trunc);
            if (!file) return false;
            file << serializeSettings(settings);
            if (!file) { file.close(); std::filesystem::remove(temporaryPath, error); return false; }
        }
        const auto backupPath = std::filesystem::path(path.string() + ".bak");
        if (std::filesystem::exists(path, error))
        {
            error.clear();
            std::filesystem::copy_file(path, backupPath, std::filesystem::copy_options::overwrite_existing, error);
            error.clear();
        }
        std::filesystem::rename(temporaryPath, path, error);
        if (error) { std::filesystem::remove(temporaryPath, error); return false; }
        return true;
    }

    HSV rgbToHsv(const Color32 color) noexcept
    {
        const float r = color.R / 255.0f;
        const float g = color.G / 255.0f;
        const float b = color.B / 255.0f;
        const float maximum = std::max({r, g, b});
        const float minimum = std::min({r, g, b});
        const float delta = maximum - minimum;
        HSV hsv{.S = maximum > 0.0f ? delta / maximum : 0.0f, .V = maximum};
        if (delta <= 0.00001f)
            return hsv;
        if (maximum == r)
            hsv.H = std::fmod((g - b) / delta, 6.0f);
        else if (maximum == g)
            hsv.H = (b - r) / delta + 2.0f;
        else
            hsv.H = (r - g) / delta + 4.0f;
        hsv.H /= 6.0f;
        if (hsv.H < 0.0f)
            hsv.H += 1.0f;
        return hsv;
    }

    Color32 hsvToRgb(float h, const float s, const float v) noexcept
    {
        h -= std::floor(h);
        const float x = h * 6.0f;
        const int sector = static_cast<int>(x);
        const float fraction = x - static_cast<float>(sector);
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - fraction * s);
        const float t = v * (1.0f - (1.0f - fraction) * s);
        float r;
        float g;
        float b;
        switch (sector % 6)
        {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
        return {
            static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f))
        };
    }

    Color32 lerpColor(const Color32 from, const Color32 to, const float amount) noexcept
    {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        const auto a = rgbToHsv(from);
        const auto b = rgbToHsv(to);
        float hueDelta = b.H - a.H;
        if (hueDelta > 0.5f)
            hueDelta -= 1.0f;
        else if (hueDelta < -0.5f)
            hueDelta += 1.0f;
        float h = a.H + hueDelta * t;
        h -= std::floor(h);
        return hsvToRgb(h, a.S + (b.S - a.S) * t, a.V + (b.V - a.V) * t);
    }

    Color32 floatColor(const std::array<float, 3>& color) noexcept
    {
        return {
            static_cast<std::uint8_t>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
            static_cast<std::uint8_t>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f))
        };
    }

    void saturate(float& r, float& g, float& b, const float saturation) noexcept
    {
        const float gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
        r = std::clamp(gray + (r - gray) * saturation, 0.0f, 255.0f);
        g = std::clamp(gray + (g - gray) * saturation, 0.0f, 255.0f);
        b = std::clamp(gray + (b - gray) * saturation, 0.0f, 255.0f);
    }


    std::unordered_map<std::string, std::string> g_ShaderMaterialValues;

    std::string shaderMaterialSerializeFloats(const std::array<float, 4>& values, const int count)
    {
        std::ostringstream stream;
        stream << std::setprecision(9);
        for (int i = 0; i < count; ++i)
        {
            if (i != 0) stream << ',';
            stream << values[static_cast<std::size_t>(i)];
        }
        return stream.str();
    }

    std::string shaderMaterialSerializeInts(const std::array<int, 4>& values, const int count)
    {
        std::ostringstream stream;
        for (int i = 0; i < count; ++i)
        {
            if (i != 0) stream << ',';
            stream << values[static_cast<std::size_t>(i)];
        }
        return stream.str();
    }

    bool parseShaderMaterialFloats(const std::string_view value, std::array<float, 4>& result, const int count)
    {
        std::array<float, 4> parsed{};
        std::size_t start = 0;
        for (int i = 0; i < count; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!parseNumber(part, parsed[static_cast<std::size_t>(i)])) return false;
            if (i + 1 < count)
            {
                if (end == std::string_view::npos) return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    bool parseShaderMaterialInts(const std::string_view value, std::array<int, 4>& result, const int count)
    {
        std::array<int, 4> parsed{};
        std::size_t start = 0;
        for (int i = 0; i < count; ++i)
        {
            const std::size_t end = value.find(',', start);
            const std::string_view part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!parseNumber(part, parsed[static_cast<std::size_t>(i)])) return false;
            if (i + 1 < count)
            {
                if (end == std::string_view::npos) return false;
                start = end + 1;
            }
            else if (end != std::string_view::npos)
                return false;
        }
        result = parsed;
        return true;
    }

    void loadShaderMaterialValueCache()
    {
        g_ShaderMaterialValues.clear();
        std::ifstream file(shaderMaterialPath());
        std::string line;
        while (std::getline(file, line))
        {
            const std::string_view whole = trimSettingValue(line);
            if (whole.empty() || whole.front() == '#' || whole.front() == ';') continue;
            const std::size_t separator = whole.find('=');
            if (separator == std::string_view::npos) continue;
            const std::string key(trimSettingValue(whole.substr(0, separator)));
            const std::string value(trimSettingValue(whole.substr(separator + 1)));
            if (!key.empty()) g_ShaderMaterialValues[key] = value;
        }
    }

    bool saveShaderMaterialValueCache()
    {
        const auto path = shaderMaterialPath();
        std::error_code error;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
        const auto temporaryPath = std::filesystem::path(path.string() + ".tmp");
        std::ofstream file(temporaryPath, std::ios::trunc);
        if (!file) return false;
        file << "# Quartz reflected shader material parameters\n";
        std::vector<std::string> keys;
        keys.reserve(g_ShaderMaterialValues.size());
        for (const auto& [key, _] : g_ShaderMaterialValues) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) file << key << '=' << g_ShaderMaterialValues[key] << '\n';
        file.close();
        if (!file) { std::filesystem::remove(temporaryPath, error); return false; }
        std::filesystem::rename(temporaryPath, path, error);
        if (error)
        {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporaryPath, path, error);
        }
        return !error;
    }

    std::string prettyUniformLabel(std::string_view name)
    {
        if (name.size() > 1 && name[0] == 'u' && std::isupper(static_cast<unsigned char>(name[1]))) name.remove_prefix(1);
        std::string result;
        result.reserve(name.size() + 8);
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const char c = name[i];
            if (i != 0 && std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1]))) result.push_back(' ');
            result.push_back(c);
        }
        if (!result.empty()) result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
        return result;
    }

    bool isEngineShaderUniform(std::string_view name)
    {
        if (const std::size_t bracket = name.find('['); bracket != std::string_view::npos) name = name.substr(0, bracket);
        static constexpr std::array<std::string_view, 20> Names{
            "uTime", "uResolution", "uBands", "uMediaColor", "uMediaAmount", "uSolidColor", "uWaveSpeed", "uFeatherRows", "uSaturation",
            "uForceFullRow", "uFullRow", "uCapsLock", "uScrollLock", "uCapsLockColor", "uScrollLockColor", "uCapsLockColorEnabled",
            "uScrollLockColorEnabled", "uKeyState", "uKeyEvents", "gl_DepthRange"
        };
        return std::find(Names.begin(), Names.end(), name) != Names.end();
    }

    std::unordered_map<std::string, ShaderUniformMetadata> parseShaderUniformMetadata(const std::string_view vertexSource, const std::string_view fragmentSource)
    {
        std::unordered_map<std::string, ShaderUniformMetadata> result;
        static const std::regex uniformPattern(R"rx(\buniform\s+[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;)rx");
        static const std::regex labelPattern(R"rx(label\s*=\s*"([^"]*)")rx", std::regex::icase);
        static const std::regex idPattern(R"(\bid\s*=\s*([A-Za-z0-9_.:-]+))", std::regex::icase);
        const auto parseSource = [&](const std::string_view source)
        {
            std::size_t offset = 0;
            while (offset < source.size())
            {
                const std::size_t end = source.find('\n', offset);
                const std::string line(source.substr(offset, end == std::string_view::npos ? source.size() - offset : end - offset));
                std::smatch match;
                if (std::regex_search(line, match, uniformPattern))
                {
                    const std::size_t uiOffset = line.find("@ui");
                    if (uiOffset != std::string::npos)
                    {
                        ShaderUniformMetadata metadata{};
                        metadata.Explicit = true;
                        const std::string options = line.substr(uiOffset + 3);
                        metadata.Hidden = std::regex_search(options, std::regex(R"(\bhidden\b)", std::regex::icase));
                        metadata.Color = std::regex_search(options, std::regex(R"(\bcolor\b)", std::regex::icase));
                        std::smatch optionMatch;
                        if (std::regex_search(options, optionMatch, labelPattern)) metadata.Label = optionMatch[1].str();
                        if (std::regex_search(options, optionMatch, idPattern)) metadata.Id = optionMatch[1].str();

                        const auto findNumber = [&](const char* key, float& destination, bool& present)
                        {
                            const std::regex pattern(std::string(R"rx(\b)rx") + key + R"rx(\s*=\s*([-+]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][-+]?[0-9]+)?))rx", std::regex::icase);
                            std::smatch numberMatch;
                            if (!std::regex_search(options, numberMatch, pattern)) return;
                            if (parseNumber(numberMatch[1].str(), destination)) present = true;
                        };
                        findNumber("min", metadata.Min, metadata.HasMin);
                        findNumber("max", metadata.Max, metadata.HasMax);
                        findNumber("step", metadata.Step, metadata.HasStep);

                        const std::regex defaultPattern(R"(\bdefault\s*=\s*([^\s]+))", std::regex::icase);
                        if (std::regex_search(options, optionMatch, defaultPattern))
                        {
                            metadata.HasDefault = parseShaderMaterialFloats(optionMatch[1].str(), metadata.Default, 1);
                            if (!metadata.HasDefault)
                            {
                                std::array<float, 4> values{};
                                for (int count = 4; count >= 2 && !metadata.HasDefault; --count)
                                {
                                    if (parseShaderMaterialFloats(optionMatch[1].str(), values, count))
                                    {
                                        metadata.Default = values;
                                        metadata.HasDefault = true;
                                    }
                                }
                            }
                        }
                        result[match[1].str()] = std::move(metadata);
                    }
                }
                if (end == std::string_view::npos) break;
                offset = end + 1;
            }
        };
        parseSource(vertexSource);
        parseSource(fragmentSource);
        return result;
    }

    int shaderUniformComponents(const GLenum type) noexcept
    {
        switch (type)
        {
        case GL_FLOAT:
        case GL_INT:
        case GL_BOOL: return 1;
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_BOOL_VEC2: return 2;
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_BOOL_VEC3: return 3;
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_BOOL_VEC4: return 4;
        default: return 0;
        }
    }

    const char* shaderUniformTypeName(const GLenum type) noexcept
    {
        switch (type)
        {
        case GL_FLOAT: return "float";
        case GL_FLOAT_VEC2: return "vec2";
        case GL_FLOAT_VEC3: return "vec3";
        case GL_FLOAT_VEC4: return "vec4";
        case GL_INT: return "int";
        case GL_INT_VEC2: return "ivec2";
        case GL_INT_VEC3: return "ivec3";
        case GL_INT_VEC4: return "ivec4";
        case GL_BOOL: return "bool";
        case GL_BOOL_VEC2: return "bvec2";
        case GL_BOOL_VEC3: return "bvec3";
        case GL_BOOL_VEC4: return "bvec4";
        default: return "unsupported";
        }
    }

    void applyShaderDiagnostics(TextEditor& editor, const std::string_view log)
    {
        editor.ClearMarkers();
        static const std::regex colonPattern(R"((?:ERROR|WARNING)?\s*:?\s*\d+:(\d+)(?::|\())", std::regex::icase);
        static const std::regex parenPattern(R"(\d+\((\d+)\))");
        std::size_t start = 0;
        while (start < log.size())
        {
            const std::size_t end = log.find('\n', start);
            std::string line(log.substr(start, end == std::string_view::npos ? log.size() - start : end - start));
            std::smatch match;
            std::size_t lineNumber = 0;
            if (std::regex_search(line, match, colonPattern) || std::regex_search(line, match, parenPattern))
            {
                try { lineNumber = static_cast<std::size_t>(std::stoul(match[1].str())); } catch (...) { lineNumber = 0; }
            }
            if (lineNumber > 0)
            {
                const bool warning = line.find("warning") != std::string::npos || line.find("WARNING") != std::string::npos;
                const ImU32 numberColor = warning ? IM_COL32(255, 195, 70, 255) : IM_COL32(255, 85, 85, 255);
                const ImU32 textColor = warning ? IM_COL32(115, 80, 10, 55) : IM_COL32(120, 20, 20, 65);
                editor.AddMarker(lineNumber - 1, numberColor, textColor, line, line);
            }
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    void updateShaderDiagnostics(ShaderEditorState& editors, const std::string& status)
    {
        if (!editors.Initialized)
            return;
        editors.Vertex.ClearMarkers();
        editors.Fragment.ClearMarkers();
        if (status.starts_with("Vertex compile error:"))
            applyShaderDiagnostics(editors.Vertex, status);
        else if (status.starts_with("Fragment compile error:"))
            applyShaderDiagnostics(editors.Fragment, status);
    }

    bool compileShaders(ShaderFramebuffer& framebuffer, ShaderEditorState& editors, const std::array<char, ShaderSourceCapacity>& vertexSource, const std::array<char, ShaderSourceCapacity>& fragmentSource)
    {
        const bool result = framebuffer.compile(vertexSource.data(), fragmentSource.data());
        updateShaderDiagnostics(editors, framebuffer.status());
        return result;
    }

    Color32 lerpColorLinear(const Color32 from, const Color32 to, const float amount) noexcept
    {
        const float t = std::clamp(amount, 0.0f, 1.0f);
        const auto mix = [t](const std::uint8_t a, const std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(static_cast<float>(a) + (static_cast<float>(b) - a) * t)); };
        return {mix(from.R, to.R), mix(from.G, to.G), mix(from.B, to.B)};
    }

    bool switchShaderPreset(ShaderFramebuffer& framebuffer, ShaderTransitionState& transition, ShaderEditorState& editors, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const int presetIndex, const double now, const float transitionSeconds, const bool persist)
    {
        if (presetIndex <= 0 || presetIndex > static_cast<int>(ShaderPresets.size())) return false;
        const auto& preset = ShaderPresets[static_cast<std::size_t>(presetIndex - 1)];
        if (std::string_view(fragmentSource.data()) == preset.FragmentSource)
        {
            settings.ShaderPresetIndex = presetIndex;
            settings.ShaderId = preset.Id;
            return true;
        }

        const std::array<char, ShaderSourceCapacity> previousSource = fragmentSource;
        transition.cancel();
        const float duration = std::clamp(transitionSeconds, 0.0f, 10.0f);
        if (duration > 0.0f && framebuffer.ready())
        {
            framebuffer.snapshotMaterialValues();
            if (transition.Previous.initialize(settings.ShaderFramebufferWidth, settings.ShaderFramebufferHeight) && transition.Previous.compile(vertexSource.data(), previousSource.data()))
            {
                transition.Active = true;
                transition.StartedAt = now;
                transition.Duration = duration;
            }
            else
                transition.cancel();
        }

        setShaderSource(fragmentSource, preset.FragmentSource);
        if (!compileShaders(framebuffer, editors, vertexSource, fragmentSource))
        {
            fragmentSource = previousSource;
            transition.cancel();
            return false;
        }

        settings.ShaderPresetIndex = presetIndex;
        settings.ShaderId = preset.Id;
        settings.BaseColorMode = 2;
        if (editors.Initialized) editors.Fragment.SetText(fragmentSource.data());
        if (persist) saveShaderSources(vertexSource, fragmentSource);
        return true;
    }

    bool switchShaderId(ShaderFramebuffer& framebuffer, ShaderTransitionState& transition, ShaderEditorState& editors, std::array<char, ShaderSourceCapacity>& vertexSource, std::array<char, ShaderSourceCapacity>& fragmentSource, VisualizerSettings& settings, const std::string_view shaderId, const double now, const float transitionSeconds, const bool persist)
    {
        const int index = shaderPresetIndexById(shaderId);
        return index > 0 && switchShaderPreset(framebuffer, transition, editors, vertexSource, fragmentSource, settings, index, now, transitionSeconds, persist);
    }


}
