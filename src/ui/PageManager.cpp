#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/pages/SpectrumPage.hpp"
#include "quartz/client/ui/pages/DevicePage.hpp"
#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/pages/QRPCPage.hpp"
#include "quartz/client/ui/pages/USBPage.hpp"
#include "quartz/client/ui/pages/InputPage.hpp"
#include "quartz/client/ui/pages/RGBPage.hpp"
#include "quartz/client/ui/pages/AudioPage.hpp"
#include "quartz/client/ui/pages/TimelinePage.hpp"
#include "quartz/client/ui/pages/FirmwarePage.hpp"
#include "quartz/client/ui/pages/PerformancePage.hpp"
#include "quartz/client/ui/pages/MatrixTimingPage.hpp"
#include "quartz/client/ui/pages/ShaderEditorPage.hpp"

#include <imgui.h>

namespace quartz::client::ui
{
    Page* PageManager::find(const std::string_view id) noexcept
    {
        for (const auto& page : _pages) if (page->id() == id) return page.get();
        return nullptr;
    }

    const Page* PageManager::find(const std::string_view id) const noexcept
    {
        for (const auto& page : _pages) if (page->id() == id) return page.get();
        return nullptr;
    }

    bool PageManager::open(const std::string_view id)
    {
        Page* page = find(id);
        if (!page) return false;
        if (page->presentation() == PagePresentation::Standalone) _standaloneId.assign(id);
        else _requestedTabId.assign(id);
        return true;
    }

    void PageManager::render(PageContext& context)
    {
        if (!_standaloneId.empty())
        {
            Page* page = find(_standaloneId);
            if (!page || page->presentation() != PagePresentation::Standalone) { _standaloneId.clear(); return; }
            page->render(context, *this);
            return;
        }

        if (!ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyResizeDown | ImGuiTabBarFlags_TabListPopupButton)) return;
        for (const auto& page : _pages)
        {
            if (page->presentation() != PagePresentation::Tab) continue;
            const ImGuiTabItemFlags flags = _requestedTabId == page->id() ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(page->title().data(), nullptr, flags))
            {
                page->render(context, *this);
                ImGui::EndTabItem();
            }
        }
        _requestedTabId.clear();
        ImGui::EndTabBar();
    }

    PageManager createDefaultPageManager()
    {
        PageManager manager;
        manager.add<VisualizerPage>();
        manager.add<SpectrumPage>();
        manager.add<DevicePage>();
        manager.add<BindingsPage>();
        manager.add<QRPCPage>();
        manager.add<USBPage>();
        manager.add<InputPage>();
        manager.add<RGBPage>();
        manager.add<AudioPage>();
        manager.add<TimelinePage>();
        manager.add<FirmwarePage>();
        manager.add<PerformancePage>();
        manager.add<MatrixTimingPage>();
        manager.add<ShaderEditorPage>();
        return manager;
    }
}
