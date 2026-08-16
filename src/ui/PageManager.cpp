#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/I18n.hpp"
#include "quartz/client/ui/pages/VisualizerPage.hpp"
#include "quartz/client/ui/pages/SpectrumPage.hpp"
#include "quartz/client/ui/pages/AudioPage.hpp"
#include "quartz/client/ui/pages/RGBPage.hpp"
#include "quartz/client/ui/pages/ShadersPage.hpp"
#include "quartz/client/ui/pages/JavaScriptPage.hpp"
#include "quartz/client/ui/pages/NativePage.hpp"
#include "quartz/client/ui/pages/MemoryScannerPage.hpp"
#include "quartz/client/ui/pages/MemoryWatchPage.hpp"
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
#include "quartz/client/ui/pages/OpcodeEditorPage.hpp"
#include <array>
#include <imgui.h>

namespace quartz::client::ui
{
    namespace
    {
        constexpr auto Sections = std::to_array<PageSection>({PageSection::Visual, PageSection::Scripting, PageSection::ReverseEngineering, PageSection::Device, PageSection::Diagnostics, PageSection::Other});
        const char* sectionName(const PageSection section) noexcept
        {
            switch (section)
            {
            case PageSection::Visual: return i18n::tr("nav.section.visual");
            case PageSection::Scripting: return i18n::tr("nav.section.scripting");
            case PageSection::ReverseEngineering: return i18n::tr("nav.section.reverseEngineering");
            case PageSection::Device: return i18n::tr("nav.section.device");
            case PageSection::Diagnostics: return i18n::tr("nav.section.diagnostics");
            case PageSection::Other: return i18n::tr("nav.section.other");
            }
            return i18n::tr("nav.section.other");
        }

        const char* pageName(const Page& page)
        {
            const std::string key = "nav." + std::string(page.id());
            const char* translated = i18n::tr(key);
            return std::string_view(translated) == key ? page.title().data() : translated;
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
        constexpr float NavigationWidth = 210.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
        if (ImGui::BeginChild("PageNavigation", ImVec2(NavigationWidth, available.y), ImGuiChildFlags_Borders))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            ImGui::Dummy({0.0f, 3.0f});
            for (const PageSection section : Sections)
            {
                bool hasPages = false;
                for (const auto& page : _pages) if (page->presentation() == PagePresentation::Tab && page->section() == section) { hasPages = true; break; }
                if (!hasPages) continue;
                ImGui::TextDisabled("%s", sectionName(section));
                ImGui::Dummy({0.0f, 1.0f});
                for (const auto& page : _pages)
                {
                    if (page->presentation() != PagePresentation::Tab || page->section() != section) continue;
                    const bool selected = page->id() == _activePageId;
                    const float selectableWidth = ImGui::GetContentRegionAvail().x;
                    ImGui::PushID(page->id().data());
                    if (ImGui::Selectable(pageName(*page), selected, ImGuiSelectableFlags_None, ImVec2(selectableWidth, 25.0f))) _activePageId.assign(page->id());
                    if (selected)
                    {
                        const ImVec2 min = ImGui::GetItemRectMin(); const ImVec2 max = ImGui::GetItemRectMax();
                        ImGui::GetWindowDrawList()->AddRectFilled({min.x, min.y + 3.0f}, {min.x + 3.0f, max.y - 3.0f}, ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
                    }
                    ImGui::PopID();
                }
                ImGui::Spacing(); ImGui::Spacing();
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
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
        manager.add<ShadersPage>();
        manager.add<SpectrumPage>();
        manager.add<AudioPage>();
        manager.add<RGBPage>();
        manager.add<JavaScriptPage>();
        manager.add<ProfilesPage>();
        manager.add<NativePage>();
        manager.add<MemoryScannerPage>();
        manager.add<MemoryWatchPage>();
        manager.add<DevicePage>();
        manager.add<InputPage>();
        manager.add<USBPage>();
        manager.add<FirmwarePage>();
        manager.add<PerformancePage>();
        manager.add<MatrixTimingPage>();
        manager.add<QRPCPage>();
        manager.add<TimelinePage>();
        manager.add<ShaderEditorPage>();
        manager.add<OpcodeEditorPage>();
        return manager;
    }
}
