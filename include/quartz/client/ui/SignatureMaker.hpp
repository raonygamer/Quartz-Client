#pragma once
#include <cstdint>
#include <sys/types.h>

namespace quartz::client::ui
{
    struct PageContext;
    class PageManager;

    void requestSignatureMaker(pid_t pid, std::uintptr_t address, int minimumInstructions = 1) noexcept;
    bool signatureMakerWantsFocus() noexcept;
    void drawSignatureMaker(PageContext& context, PageManager& manager);
}
