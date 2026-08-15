#pragma once
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"

namespace quartz::client
{
    struct PreviewRect
    {
        std::uint8_t Row;
        std::uint8_t Column;
        float X;
        float Y;
        float Width;
        float Height;
    };

    struct RuntimeMemoryInspectorState
    {
        pid_t Pid = 0;
        std::uintptr_t Address = 0;
        int ReadSize = 128;
        std::vector<std::uint8_t> Original;
        std::vector<std::uint8_t> Patched;
        std::array<char, 16 * 1024> HexEdit{};
        std::string Status;
        TextEditor Disassembly;
        bool EditorInitialized = false;
        int WriteConfirm = 0;
    };

}
