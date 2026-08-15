#include "quartz/client/ui/pages/InputPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void InputPage::render(PageContext& context, PageManager& manager)
    {
        const auto& keyboardInput = context.keyboardInput;
        const auto& reactiveKeys = context.reactiveKeys;
        const auto& inputAnalytics = context.inputAnalytics;

        (void)manager;
        drawInputAnalyzerPage(keyboardInput, reactiveKeys, inputAnalytics);
    }
}
