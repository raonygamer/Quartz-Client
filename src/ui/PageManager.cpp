#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/pages/SpectrumPage.hpp"
#include "quartz/client/ui/pages/AudioPage.hpp"
#include "quartz/client/ui/pages/RGBPage.hpp"
#include "quartz/client/ui/pages/BindingsPage.hpp"
#include "quartz/client/ui/pages/ControlsPage.hpp"
#include "quartz/client/ui/pages/ObjectsPage.hpp"
#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/pages/ValueBankPage.hpp"
#include "quartz/client/ui/pages/ProfilesPage.hpp"
#include "quartz/client/ui/pages/DevicePage.hpp"
#include "quartz/client/ui/pages/InputPage.hpp"
#include "quartz/client/ui/pages/USBPage.hpp"
#include "quartz/client/ui/pages/QRPCPage.hpp"
#include "quartz/client/ui/pages/FirmwarePage.hpp"
#include "quartz/client/ui/pages/TimelinePage.hpp"
#include "quartz/client/ui/pages/PerformancePage.hpp"
#include "quartz/client/ui/pages/MatrixTimingPage.hpp"
#include "quartz/client/ui/pages/ShaderEditorPage.hpp"
#include <array>
#include <imgui.h>

namespace quartz::client::ui
{
    namespace
    {
        constexpr auto Sections = std::to_array<PageSection>({PageSection::Visual, PageSection::Runtime, PageSection::Device, PageSection::Diagnostics, PageSection::Other});
        const char* sectionName(const PageSection section) noexcept
        {
            switch (section)
            {
            case PageSection::Visual: return "VISUAL";
            case PageSection::Runtime: return "RUNTIME";
            case PageSection::Device: return "DEVICE";
            case PageSection::Diagnostics: return "DIAGNOSTICS";
            case PageSection::Other: return "OTHER";
            }
            return "OTHER";
        }
    }

    Page* PageManager::find(const std::string_view id) noexcept { for (const auto& page : _pages) if (page->id() == id) return page.get(); return nullptr; }
    const Page* PageManager::find(const std::string_view id) const noexcept { for (const auto& page : _pages) if (page->id() == id) return page.get(); return nullptr; }

    bool PageManager::open(const std::string_view id)
    {
        Page* page = find(id);
        if (!page) return false;
        if (page->presentation() == PagePresentation::Standalone) _standaloneId.assign(id);
        else _activePageId.assign(id);
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
        Page* active = find(_activePageId);
        if (!active || active->presentation() != PagePresentation::Tab)
        {
            active = nullptr;
            for (const auto& page : _pages) if (page->presentation() == PagePresentation::Tab) { active = page.get(); _activePageId.assign(page->id()); break; }
        }
        if (!active) return;

        const ImVec2 available = ImGui::GetContentRegionAvail();
        constexpr float NavigationWidth = 158.0f;
        if (ImGui::BeginChild("PageNavigation", ImVec2(NavigationWidth, available.y), ImGuiChildFlags_Borders))
        {
            for (const PageSection section : Sections)
            {
                bool hasPages = false;
                for (const auto& page : _pages) if (page->presentation() == PagePresentation::Tab && page->section() == section) { hasPages = true; break; }
                if (!hasPages) continue;
                ImGui::TextDisabled("%s", sectionName(section));
                for (const auto& page : _pages)
                {
                    if (page->presentation() != PagePresentation::Tab || page->section() != section) continue;
                    const bool selected = page->id() == _activePageId;
                    if (ImGui::Selectable(page->title().data(), selected, ImGuiSelectableFlags_None, ImVec2(-1.0f, 0.0f))) _activePageId.assign(page->id());
                }
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("PageContent", ImVec2(0.0f, available.y), ImGuiChildFlags_None))
        {
            if (Page* selected = find(_activePageId); selected && selected->presentation() == PagePresentation::Tab) selected->render(context, *this);
        }
        ImGui::EndChild();
    }

    PageManager createDefaultPageManager()
    {
        PageManager manager;
        manager.add<VisualizerPage>();
        manager.add<SpectrumPage>();
        manager.add<AudioPage>();
        manager.add<RGBPage>();
        manager.add<BindingsPage>();
        manager.add<ControlsPage>();
        manager.add<ObjectsPage>();
        manager.add<NativePage>();
        manager.add<ValueBankPage>();
        manager.add<ProfilesPage>();
        manager.add<DevicePage>();
        manager.add<InputPage>();
        manager.add<USBPage>();
        manager.add<QRPCPage>();
        manager.add<FirmwarePage>();
        manager.add<TimelinePage>();
        manager.add<PerformancePage>();
        manager.add<MatrixTimingPage>();
        manager.add<ShaderEditorPage>();
        return manager;
    }
}
