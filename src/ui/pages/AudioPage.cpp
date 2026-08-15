#include "quartz/client/ui/pages/AudioPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void AudioPage::render(PageContext& context, PageManager& manager)
    {
        auto& audio = context.audio;
        auto& settings = context.settings;
        const auto& analysisBands = context.analysisBands;
        const auto& mappedBands = context.mappedBands;
        const auto& smoothedBands = context.smoothedBands;
        const auto& autoGain = context.autoGain;
        const auto& audioLevel = context.audioLevel;

        (void)manager;
        drawAudioLabPage(audio, audioLevel, autoGain, settings, analysisBands, mappedBands, smoothedBands);
    }
}
