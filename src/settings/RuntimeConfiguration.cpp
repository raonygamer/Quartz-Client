#include "quartz/client/settings/RuntimeConfiguration.hpp"
#include "quartz/client/Functions.hpp"
#include "quartz/client/native/SignatureScanner.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace quartz::client
{
    namespace
    {
        std::filesystem::path configurationPath() { return settingsPath().parent_path() / "configuration.ini"; }

        void apply(RuntimeConfiguration& configuration) noexcept
        {
            configuration.SignatureScanChunkBytes = normalizeSignatureScanChunkBytes(configuration.SignatureScanChunkBytes);
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
                if (key != "SignatureScanChunkBytes") continue;
                std::size_t parsed = 0; const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed); if (ec == std::errc{} && ptr == value.data() + value.size()) configuration.SignatureScanChunkBytes = parsed;
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
            std::ofstream file(path, std::ios::trunc); if (file) file << "SignatureScanChunkBytes=" << configuration.SignatureScanChunkBytes << '\n';
        }
        catch (...) {}
    }

    void resetRuntimeConfiguration() noexcept
    {
        runtimeConfiguration() = {}; saveRuntimeConfiguration();
    }
}
