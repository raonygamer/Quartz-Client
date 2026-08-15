#pragma once
#include "quartz/client/Functions.hpp"
#include "quartz/client/settings/VisualizerSettings.hpp"
#include "quartz/client/input/Input.hpp"
#include "quartz/client/shader/ShaderMaterial.hpp"

namespace quartz::client
{
    class ShaderFramebuffer
    {
    public:
        ~ShaderFramebuffer() { shutdown(); }

        bool initialize(const int width = DefaultShaderWidth, const int height = DefaultShaderHeight) noexcept
        {
            if (_framebuffer == 0)
            {
                glGenFramebuffers(1, &_framebuffer);
                glGenTextures(1, &_texture);
                glGenVertexArrays(1, &_vao);
                if (_framebuffer == 0 || _texture == 0 || _vao == 0)
                {
                    _status = "Failed to create OpenGL shader framebuffer objects";
                    shutdown();
                    return false;
                }
                glBindTexture(GL_TEXTURE_2D, _texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            if (_width == width && _height == height && !_pixels.empty())
                return true;
            return regenerate(width, height);
        }

        bool regenerate(int width, int height) noexcept
        {
            width = std::clamp(width, static_cast<int>(Columns), MaxShaderDimension);
            height = std::clamp(height, static_cast<int>(Rows), MaxShaderDimension);
            if (_framebuffer == 0)
                return initialize(width, height);

            GLint previousFramebuffer = 0;
            GLint previousTexture = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

            glBindTexture(GL_TEXTURE_2D, _texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                _status = "Shader framebuffer is incomplete";
                return false;
            }
            _width = width;
            _height = height;
            _pixels.resize(static_cast<std::size_t>(_width) * _height * 4);
            _status = "Shader framebuffer regenerated";
            return true;
        }

        bool compile(const std::string_view vertexSource, const std::string_view fragmentSource)
        {
            if (!initialize(_width > 0 ? _width : DefaultShaderWidth, _height > 0 ? _height : DefaultShaderHeight))
                return false;
            const GLuint vertex = compileStage(GL_VERTEX_SHADER, vertexSource);
            if (vertex == 0)
                return false;
            const GLuint fragment = compileStage(GL_FRAGMENT_SHADER, fragmentSource);
            if (fragment == 0)
            {
                glDeleteShader(vertex);
                return false;
            }

            const GLuint program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            glDeleteShader(vertex);
            glDeleteShader(fragment);

            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE)
            {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
                glGetProgramInfoLog(program, length, nullptr, log.data());
                _status = "Link error:\n" + log;
                glDeleteProgram(program);
                return false;
            }

            stashMaterialValues();
            if (_program != 0)
                glDeleteProgram(_program);
            _program = program;
            reflectMaterialParameters(vertexSource, fragmentSource);
            _status = "Shaders compiled successfully";
            return true;
        }

        bool render(const double time, const std::array<float, Columns>& bands, const VisualizerSettings& settings, const std::optional<Color32> mediaColor, const float mediaAmount, const ReactiveKeyState& reactiveKeys, std::array<Color32, MatrixSize>& framebuffer)
        {
            if (_program == 0 || _framebuffer == 0 || _width <= 0 || _height <= 0)
                return false;

            GLint previousFramebuffer = 0;
            GLint previousProgram = 0;
            GLint previousVao = 0;
            GLint previousViewport[4]{};
            GLfloat previousClearColor[4]{};
            GLint previousPackAlignment = 4;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
            glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
            const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
            const GLboolean ditherEnabled = glIsEnabled(GL_DITHER);
            const GLboolean framebufferSrgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);

            glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
            glViewport(0, 0, _width, _height);
            glDisable(GL_BLEND);
            glDisable(GL_DITHER);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(_program);
            glBindVertexArray(_vao);

            uniform1f("uTime", static_cast<float>(time));
            uniform2f("uResolution", static_cast<float>(_width), static_cast<float>(_height));
            const GLint bandsLocation = glGetUniformLocation(_program, "uBands");
            if (bandsLocation >= 0)
                glUniform1fv(bandsLocation, static_cast<GLsizei>(Columns), bands.data());
            const Color32 media = mediaColor.value_or(Color32{0, 0, 0});
            uniform3f("uMediaColor", media.R / 255.0f, media.G / 255.0f, media.B / 255.0f);
            uniform1f("uMediaAmount", std::clamp(mediaAmount * settings.MediaColorBlend, 0.0f, 1.0f));
            uniform3f("uSolidColor", settings.SolidColor[0], settings.SolidColor[1], settings.SolidColor[2]);
            uniform1f("uWaveSpeed", settings.WaveSpeed);
            uniform1f("uFeatherRows", settings.FeatherRows);
            uniform1f("uSaturation", settings.Saturation);
            uniform1i("uForceFullRow", settings.ForceFullRow ? 1 : 0);
            uniform1i("uFullRow", settings.FullRow);
            uniform1f("uCapsLock", settings.ShaderKeyStateUniforms && reactiveKeys.CapsLockActive ? 1.0f : 0.0f);
            uniform1f("uScrollLock", settings.ShaderKeyStateUniforms && reactiveKeys.ScrollLockActive ? 1.0f : 0.0f);
            const GLint keyStateLocation = glGetUniformLocation(_program, "uKeyState[0]");
            if (keyStateLocation >= 0)
            {
                if (settings.ShaderKeyStateUniforms)
                    glUniform1fv(keyStateLocation, static_cast<GLsizei>(MatrixSize), reactiveKeys.Down.data());
                else
                {
                    static constexpr std::array<float, MatrixSize> EmptyKeyState{};
                    glUniform1fv(keyStateLocation, static_cast<GLsizei>(MatrixSize), EmptyKeyState.data());
                }
            }
            const GLint keyEventsLocation = glGetUniformLocation(_program, "uKeyEvents[0]");
            if (keyEventsLocation >= 0)
            {
                std::array<float, ReactiveKeyState::EventCount * 4> events{};
                if (settings.ShaderKeyStateUniforms)
                {
                    for (std::size_t i = 0; i < reactiveKeys.Events.size(); ++i)
                    {
                        events[i * 4 + 0] = reactiveKeys.Events[i].Column;
                        events[i * 4 + 1] = reactiveKeys.Events[i].Row;
                        events[i * 4 + 2] = reactiveKeys.Events[i].Time;
                        events[i * 4 + 3] = reactiveKeys.Events[i].Valid;
                    }
                }
                glUniform4fv(keyEventsLocation, static_cast<GLsizei>(ReactiveKeyState::EventCount), events.data());
            }
            uniform3f("uCapsLockColor", settings.ShaderCapsLockColor[0], settings.ShaderCapsLockColor[1], settings.ShaderCapsLockColor[2]);
            uniform3f("uScrollLockColor", settings.ShaderScrollLockColor[0], settings.ShaderScrollLockColor[1], settings.ShaderScrollLockColor[2]);
            uniform1i("uCapsLockColorEnabled", settings.ShaderCapsLockColorEnabled ? 1 : 0);
            uniform1i("uScrollLockColorEnabled", settings.ShaderScrollLockColorEnabled ? 1 : 0);

            applyMaterialParameters();
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, _width, _height, GL_RGBA, GL_UNSIGNED_BYTE, _pixels.data());

            const auto samplePixel = [&](const int x, const int y) -> Color32
            {
                const int safeX = std::clamp(x, 0, _width - 1);
                const int safeY = std::clamp(y, 0, _height - 1);
                const std::size_t offset = (static_cast<std::size_t>(safeY) * _width + safeX) * 4;
                return {_pixels[offset + 0], _pixels[offset + 1], _pixels[offset + 2]};
            };
            const auto averageRegion = [&](const int x, const int y, const int width, const int height) -> Color32
            {
                const int safeWidth = std::max(width, 1);
                const int safeHeight = std::max(height, 1);
                std::uint64_t r = 0, g = 0, b = 0;
                for (int py = 0; py < safeHeight; ++py)
                {
                    for (int px = 0; px < safeWidth; ++px)
                    {
                        const auto color = samplePixel(x + px, y + py);
                        r += color.R;
                        g += color.G;
                        b += color.B;
                    }
                }
                const std::uint64_t samples = static_cast<std::uint64_t>(safeWidth) * safeHeight;
                return {
                    static_cast<std::uint8_t>((r + samples / 2) / samples),
                    static_cast<std::uint8_t>((g + samples / 2) / samples),
                    static_cast<std::uint8_t>((b + samples / 2) / samples)
                };
            };

            for (std::size_t row = 0; row < Rows; ++row)
            {
                // OpenGL's row zero is the bottom; logical framebuffer row zero is the top.
                const int invertedRow = static_cast<int>(Rows) - 1 - static_cast<int>(row);
                const int sourceY0 = invertedRow * _height / static_cast<int>(Rows);
                const int sourceY1 = (invertedRow + 1) * _height / static_cast<int>(Rows);
                const int blockHeight = std::max(sourceY1 - sourceY0, 1);
                for (std::size_t column = 0; column < Columns; ++column)
                {
                    const int sourceX0 = static_cast<int>(column) * _width / static_cast<int>(Columns);
                    const int sourceX1 = (static_cast<int>(column) + 1) * _width / static_cast<int>(Columns);
                    const int blockWidth = std::max(sourceX1 - sourceX0, 1);
                    switch (settings.ShaderDownsampleMode)
                    {
                    case 1:
                    {
                        const int sampleWidth = std::min(4, blockWidth);
                        const int sampleHeight = std::min(4, blockHeight);
                        const int sampleX = sourceX0 + (blockWidth - sampleWidth) / 2;
                        const int sampleY = sourceY0 + (blockHeight - sampleHeight) / 2;
                        framebuffer[row * Columns + column] = averageRegion(sampleX, sampleY, sampleWidth, sampleHeight);
                        break;
                    }
                    case 2:
                        framebuffer[row * Columns + column] = samplePixel(sourceX0 + blockWidth / 2, sourceY0 + blockHeight / 2);
                        break;
                    default:
                        framebuffer[row * Columns + column] = averageRegion(sourceX0, sourceY0, blockWidth, blockHeight);
                        break;
                    }
                }
            }

            const auto indicatorColor = [](const std::array<float, 3>& color) -> Color32
            {
                return {
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
                    static_cast<std::uint8_t>(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f))
                };
            };

            // Built-in shaders consume the same uniforms, and this final logical-LED override keeps
            // the indicator colors exact even when the shader surface is supersampled/downsampled.
            if (settings.ShaderKeyStateUniforms && settings.ShaderCapsLockColorEnabled && reactiveKeys.CapsLockActive)
                framebuffer[3 * Columns + 0] = indicatorColor(settings.ShaderCapsLockColor);
            if (settings.ShaderKeyStateUniforms && settings.ShaderScrollLockColorEnabled && reactiveKeys.ScrollLockActive)
                framebuffer[0 * Columns + 14] = indicatorColor(settings.ShaderScrollLockColor);

            glBindVertexArray(static_cast<GLuint>(previousVao));
            glUseProgram(static_cast<GLuint>(previousProgram));
            if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            if (ditherEnabled) glEnable(GL_DITHER); else glDisable(GL_DITHER);
            if (framebufferSrgbEnabled) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
            glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
            glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
            glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
            return true;
        }

        void shutdown() noexcept
        {
            if (_program != 0) glDeleteProgram(_program);
            if (_vao != 0) glDeleteVertexArrays(1, &_vao);
            if (_texture != 0) glDeleteTextures(1, &_texture);
            if (_framebuffer != 0) glDeleteFramebuffers(1, &_framebuffer);
            _program = 0;
            _vao = 0;
            _texture = 0;
            _framebuffer = 0;
            _width = 0;
            _height = 0;
            _pixels.clear();
        }

        bool ready() const noexcept { return _program != 0; }
        int width() const noexcept { return _width; }
        int height() const noexcept { return _height; }
        const std::string& status() const noexcept { return _status; }
        std::vector<ShaderMaterialParameter>& materialParameters() noexcept { return _materialParameters; }
        const std::vector<ShaderMaterialParameter>& materialParameters() const noexcept { return _materialParameters; }
        std::uint64_t materialRevision() const noexcept { return _materialRevision; }
        void markMaterialChanged() noexcept { ++_materialRevision; }
        void snapshotMaterialValues() { stashMaterialValues(); }
        bool saveMaterialValues()
        {
            stashMaterialValues();
            return saveShaderMaterialValueCache();
        }

        bool setMaterialParameter(const std::string_view id, const int component, const float value) noexcept
        {
            for (auto& parameter : _materialParameters)
            {
                if (parameter.PersistenceKey != id && parameter.Name != id) continue;
                const int index = std::clamp(component, 0, std::max(parameter.Components - 1, 0));
                if (parameter.Integer || parameter.Boolean)
                    parameter.IntValue[static_cast<std::size_t>(index)] = parameter.Boolean ? (value >= 0.5f ? 1 : 0) : static_cast<int>(std::lround(value));
                else
                    parameter.FloatValue[static_cast<std::size_t>(index)] = value;
                return true;
            }
            return false;
        }

        const ShaderMaterialParameter* findMaterialParameter(const std::string_view id) const noexcept
        {
            for (const auto& parameter : _materialParameters)
                if (parameter.PersistenceKey == id || parameter.Name == id) return &parameter;
            return nullptr;
        }

    private:
        void stashMaterialValues()
        {
            for (const auto& parameter : _materialParameters)
            {
                g_ShaderMaterialValues[parameter.PersistenceKey] = parameter.Integer || parameter.Boolean
                    ? shaderMaterialSerializeInts(parameter.IntValue, parameter.Components)
                    : shaderMaterialSerializeFloats(parameter.FloatValue, parameter.Components);
            }
        }

        void reflectMaterialParameters(const std::string_view vertexSource, const std::string_view fragmentSource)
        {
            _materialParameters.clear();
            if (_program == 0) return;
            const auto metadata = parseShaderUniformMetadata(vertexSource, fragmentSource);
            GLint count = 0;
            GLint maxNameLength = 0;
            glGetProgramiv(_program, GL_ACTIVE_UNIFORMS, &count);
            glGetProgramiv(_program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
            std::vector<char> nameBuffer(static_cast<std::size_t>(std::max(maxNameLength, 2)), '\0');
            for (GLint index = 0; index < count; ++index)
            {
                GLsizei nameLength = 0;
                GLint arraySize = 0;
                GLenum type = 0;
                glGetActiveUniform(_program, static_cast<GLuint>(index), static_cast<GLsizei>(nameBuffer.size()), &nameLength, &arraySize, &type, nameBuffer.data());
                if (nameLength <= 0 || arraySize != 1) continue;
                std::string name(nameBuffer.data(), static_cast<std::size_t>(nameLength));
                if (name.ends_with("[0]")) name.resize(name.size() - 3);
                const auto metadataIt = metadata.find(name);
                const ShaderUniformMetadata* ui = metadataIt == metadata.end() ? nullptr : &metadataIt->second;
                if (ui && ui->Hidden) continue;
                if (isEngineShaderUniform(name) && (!ui || !ui->Explicit)) continue;

                const int components = shaderUniformComponents(type);
                if (components == 0) continue;
                const GLint location = glGetUniformLocation(_program, name.c_str());
                if (location < 0) continue;

                ShaderMaterialParameter parameter{};
                parameter.Name = name;
                parameter.Label = ui && !ui->Label.empty() ? ui->Label : prettyUniformLabel(name);
                parameter.PersistenceKey = ui && !ui->Id.empty() ? ui->Id : name;
                parameter.Type = type;
                parameter.Location = location;
                parameter.Components = components;
                parameter.Integer = type == GL_INT || type == GL_INT_VEC2 || type == GL_INT_VEC3 || type == GL_INT_VEC4;
                parameter.Boolean = type == GL_BOOL || type == GL_BOOL_VEC2 || type == GL_BOOL_VEC3 || type == GL_BOOL_VEC4;
                parameter.Color = ui && ui->Color && !parameter.Integer && !parameter.Boolean && (components == 3 || components == 4);
                parameter.HasMin = ui && ui->HasMin;
                parameter.HasMax = ui && ui->HasMax;
                parameter.Min = ui ? ui->Min : 0.0f;
                parameter.Max = ui ? ui->Max : 1.0f;
                parameter.Step = ui && ui->HasStep ? std::max(ui->Step, 0.000001f) : (parameter.Integer || parameter.Boolean ? 1.0f : 0.01f);

                if (parameter.Integer || parameter.Boolean)
                {
                    glGetUniformiv(_program, location, parameter.IntValue.data());
                    parameter.IntDefault = parameter.IntValue;
                    if (ui && ui->HasDefault)
                    {
                        for (int component = 0; component < components; ++component)
                            parameter.IntValue[static_cast<std::size_t>(component)] = parameter.IntDefault[static_cast<std::size_t>(component)] = static_cast<int>(std::lround(ui->Default[static_cast<std::size_t>(component)]));
                    }
                    if (const auto saved = g_ShaderMaterialValues.find(parameter.PersistenceKey); saved != g_ShaderMaterialValues.end())
                    {
                        std::array<int, 4> values{};
                        if (parseShaderMaterialInts(saved->second, values, components)) parameter.IntValue = values;
                    }
                }
                else
                {
                    glGetUniformfv(_program, location, parameter.FloatValue.data());
                    parameter.FloatDefault = parameter.FloatValue;
                    if (ui && ui->HasDefault)
                    {
                        for (int component = 0; component < components; ++component)
                            parameter.FloatValue[static_cast<std::size_t>(component)] = parameter.FloatDefault[static_cast<std::size_t>(component)] = ui->Default[static_cast<std::size_t>(component)];
                    }
                    if (const auto saved = g_ShaderMaterialValues.find(parameter.PersistenceKey); saved != g_ShaderMaterialValues.end())
                    {
                        std::array<float, 4> values{};
                        if (parseShaderMaterialFloats(saved->second, values, components)) parameter.FloatValue = values;
                    }
                }
                _materialParameters.push_back(std::move(parameter));
            }
            std::sort(_materialParameters.begin(), _materialParameters.end(), [](const auto& a, const auto& b) { return a.Label < b.Label; });
        }

        void applyMaterialParameters() const noexcept
        {
            for (const auto& parameter : _materialParameters)
            {
                if (parameter.Location < 0) continue;
                if (parameter.Boolean || parameter.Integer)
                {
                    switch (parameter.Components)
                    {
                    case 1: glUniform1i(parameter.Location, parameter.IntValue[0]); break;
                    case 2: glUniform2iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    case 3: glUniform3iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    case 4: glUniform4iv(parameter.Location, 1, parameter.IntValue.data()); break;
                    default: break;
                    }
                }
                else
                {
                    switch (parameter.Components)
                    {
                    case 1: glUniform1f(parameter.Location, parameter.FloatValue[0]); break;
                    case 2: glUniform2fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    case 3: glUniform3fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    case 4: glUniform4fv(parameter.Location, 1, parameter.FloatValue.data()); break;
                    default: break;
                    }
                }
            }
        }

        GLuint compileStage(const GLenum type, const std::string_view source)
        {
            const GLuint shader = glCreateShader(type);
            const char* data = source.data();
            const GLint length = static_cast<GLint>(source.size());
            glShaderSource(shader, 1, &data, &length);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE)
                return shader;
            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
            glGetShaderInfoLog(shader, logLength, nullptr, log.data());
            _status = (type == GL_VERTEX_SHADER ? "Vertex compile error:\n" : "Fragment compile error:\n") + log;
            glDeleteShader(shader);
            return 0;
        }

        void uniform1f(const char* name, const float value) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform1f(location, value);
        }
        void uniform1i(const char* name, const int value) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform1i(location, value);
        }
        void uniform2f(const char* name, const float x, const float y) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform2f(location, x, y);
        }
        void uniform3f(const char* name, const float x, const float y, const float z) const noexcept
        {
            const GLint location = glGetUniformLocation(_program, name);
            if (location >= 0) glUniform3f(location, x, y, z);
        }

        GLuint _framebuffer = 0;
        GLuint _texture = 0;
        GLuint _vao = 0;
        GLuint _program = 0;
        int _width = 0;
        int _height = 0;
        std::vector<std::uint8_t> _pixels;
        std::vector<ShaderMaterialParameter> _materialParameters;
        std::uint64_t _materialRevision = 0;
        std::string _status = "Shader not compiled";
    };
}
