#pragma once
#include <cstddef>

namespace quartz::client
{
    struct RuntimeConfiguration
    {
        std::size_t SignatureScanChunkBytes = 4ULL * 1024ULL * 1024ULL;
    };

    RuntimeConfiguration& runtimeConfiguration() noexcept;
    void loadRuntimeConfiguration() noexcept;
    void saveRuntimeConfiguration() noexcept;
    void resetRuntimeConfiguration() noexcept;
}
