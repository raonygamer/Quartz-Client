#pragma once
#include "quartz/client/runtime/RuntimeTypes.hpp"
#include <cstddef>
#include <vector>

namespace quartz::client::ui
{
    bool drawProcessPicker(const char* id, std::vector<RuntimeProcessInfo>& processes, pid_t& pid, char* search, std::size_t searchSize, float comboWidth = 420.0f);
    pid_t sharedReverseEngineeringProcess() noexcept;
    void setSharedReverseEngineeringProcess(pid_t pid) noexcept;
}
