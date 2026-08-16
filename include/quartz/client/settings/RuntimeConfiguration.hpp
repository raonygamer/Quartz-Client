#pragma once
#include <cstddef>

namespace quartz::client
{
    struct RuntimeConfiguration
    {
        std::size_t SignatureScanChunkBytes = 4ULL * 1024ULL * 1024ULL;
        float DisassemblyRefreshHz = 0.5f;
        float RawBytesRefreshHz = 0.25f;
        bool AssemblerFillNops = true;
        bool AssemblerWholeInstructions = true;
        bool AssemblerConsumeFollowing = false;
        bool AssemblerVerifyBeforeWrite = true;
        bool AssemblerRequireConfirmation = true;
        bool AssemblerAutoRedisassemble = true;
        bool AssemblerKeepOriginalBytes = true;
    };

    RuntimeConfiguration& runtimeConfiguration() noexcept;
    void loadRuntimeConfiguration() noexcept;
    void saveRuntimeConfiguration() noexcept;
    void resetRuntimeConfiguration() noexcept;
}
