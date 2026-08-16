#include "quartz/client/ui/pages/USBPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"
#include "quartz/client/ui/I18n.hpp"

namespace quartz::client::ui
{
    void USBPage::render(PageContext& context, PageManager& manager)
    {
        auto& usb = context.usb;
        auto& settings = context.settings;
        auto& runtimeBindings = context.runtimeBindings;
        (void)manager;

        ImGui::TextWrapped("%s", i18n::tr("usb.description"));
        ImGui::SeparatorText(i18n::tr("usb.connection"));
        const bool connected = usb.isConnected();
        ImGui::TextColored(connected ? ImVec4(0.35f, 0.86f, 0.58f, 1.0f) : ImVec4(0.72f, 0.74f, 0.78f, 1.0f), "%s", i18n::tr(connected ? "usb.connected" : "usb.disconnected"));
        ImGui::SameLine();
        if (!connected && ImGui::Button(i18n::tr("usb.connect"))) usb.connect();
        if (connected)
        {
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("usb.disconnect"))) { settings.AutoReconnect = false; usb.disconnect(); }
        }
        ImGui::SameLine(); ImGui::Checkbox(i18n::tr("usb.autoReconnect"), &settings.AutoReconnect);
        ImGui::TextDisabled(i18n::tr("usb.deviceFirmware"), VendorId, ProductId, FirmwareVersion);
        ImGui::Text(i18n::tr("usb.frames"), static_cast<unsigned long long>(context.sentFrames), static_cast<unsigned long long>(context.droppedFrames));
        if (!connected && usb.lastError() != LIBUSB_SUCCESS) ImGui::TextDisabled("libusb: %s", libusb_error_name(usb.lastError()));
        ImGui::Spacing();
        drawUSBProfilerPage(usb, runtimeBindings);
    }
}
