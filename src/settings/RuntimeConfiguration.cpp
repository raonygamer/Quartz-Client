#include "quartz/client/settings/RuntimeConfiguration.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace quartz::client
{
    namespace
    {
        std::filesystem::path configurationPath() { return settingsPath().parent_path() / "configuration.ini"; }

        bool parseBoolValue(const std::string_view value, bool& output) noexcept
        {
            if (value == "1" || value == "true") { output = true; return true; }
            if (value == "0" || value == "false") { output = false; return true; }
            return false;
        }

        bool parseFloatValue(const std::string_view value, float& output) noexcept
        {
            std::string text(value); char* end = nullptr; const float parsed = std::strtof(text.c_str(), &end);
            if (!end || end != text.c_str() + text.size() || !std::isfinite(parsed)) return false;
            output = parsed; return true;
        }

        void apply(RuntimeConfiguration& configuration) noexcept
        {
            configuration.SignatureScanChunkBytes = normalizeSignatureScanChunkBytes(configuration.SignatureScanChunkBytes);
            configuration.DisassemblyRefreshHz = std::clamp(configuration.DisassemblyRefreshHz, 0.0f, 30.0f);
            configuration.RawBytesRefreshHz = std::clamp(configuration.RawBytesRefreshHz, 0.0f, 30.0f);
            setSignatureScanChunkBytes(configuration.SignatureScanChunkBytes);
        }
    }

    RuntimeConfiguration& runtimeConfiguration() noexcept
    {
        static RuntimeConfiguration configuration;
        return configuration;
    }

    void loadRuntimeConfiguration() noexcept
    {
        auto& configuration = runtimeConfiguration();
        try
        {
            std::ifstream file(configurationPath());
            std::string line;
            while (std::getline(file, line))
            {
                const std::string_view text(line); const auto separator = text.find('='); if (separator == std::string_view::npos) continue;
                const std::string_view key = text.substr(0, separator), value = text.substr(separator + 1);
                if (key == "SignatureScanChunkBytes")
                {
                    std::size_t parsed = 0; const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed); if (ec == std::errc{} && ptr == value.data() + value.size()) configuration.SignatureScanChunkBytes = parsed;
                }
                else if (key == "DisassemblyRefreshHz") parseFloatValue(value, configuration.DisassemblyRefreshHz);
                else if (key == "RawBytesRefreshHz") parseFloatValue(value, configuration.RawBytesRefreshHz);
                else if (key == "AssemblerFillNops") parseBoolValue(value, configuration.AssemblerFillNops);
                else if (key == "AssemblerWholeInstructions") parseBoolValue(value, configuration.AssemblerWholeInstructions);
                else if (key == "AssemblerConsumeFollowing") parseBoolValue(value, configuration.AssemblerConsumeFollowing);
                else if (key == "AssemblerVerifyBeforeWrite") parseBoolValue(value, configuration.AssemblerVerifyBeforeWrite);
                else if (key == "AssemblerRequireConfirmation") parseBoolValue(value, configuration.AssemblerRequireConfirmation);
                else if (key == "AssemblerAutoRedisassemble") parseBoolValue(value, configuration.AssemblerAutoRedisassemble);
                else if (key == "AssemblerKeepOriginalBytes") parseBoolValue(value, configuration.AssemblerKeepOriginalBytes);
            }
        }
        catch (...) {}
        apply(configuration);
    }

    void saveRuntimeConfiguration() noexcept
    {
        auto& configuration = runtimeConfiguration(); apply(configuration);
        try
        {
            std::error_code ec; const auto path = configurationPath(); std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream file(path, std::ios::trunc);
            if (file)
            {
                file << "SignatureScanChunkBytes=" << configuration.SignatureScanChunkBytes << '\n';
                file << "DisassemblyRefreshHz=" << configuration.DisassemblyRefreshHz << '\n';
                file << "RawBytesRefreshHz=" << configuration.RawBytesRefreshHz << '\n';
                file << "AssemblerFillNops=" << configuration.AssemblerFillNops << '\n';
                file << "AssemblerWholeInstructions=" << configuration.AssemblerWholeInstructions << '\n';
                file << "AssemblerConsumeFollowing=" << configuration.AssemblerConsumeFollowing << '\n';
                file << "AssemblerVerifyBeforeWrite=" << configuration.AssemblerVerifyBeforeWrite << '\n';
                file << "AssemblerRequireConfirmation=" << configuration.AssemblerRequireConfirmation << '\n';
                file << "AssemblerAutoRedisassemble=" << configuration.AssemblerAutoRedisassemble << '\n';
                file << "AssemblerKeepOriginalBytes=" << configuration.AssemblerKeepOriginalBytes << '\n';
            }
        }
        catch (...) {}
    }

    void resetRuntimeConfiguration() noexcept
    {
        runtimeConfiguration() = {}; saveRuntimeConfiguration();
    }

    namespace
    {
        struct RuntimeConfigurationBootstrap { RuntimeConfigurationBootstrap() { loadRuntimeConfiguration(); } };
        const RuntimeConfigurationBootstrap RuntimeConfigurationInitializer{};
    }
}
