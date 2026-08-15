#pragma once

namespace quartz::client::ui
{
    struct PageContext;
    class PageManager;
    void drawManualMemoryWatch(PageContext& context, PageManager& manager);
    void drawQuickSignatureSearch(PageContext& context, PageManager& manager);
    void drawObjectModelDebugger(PageContext& context, PageManager& manager);
}
