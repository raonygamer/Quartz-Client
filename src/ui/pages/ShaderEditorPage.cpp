#include "quartz/client/ui/pages/ShaderEditorPage.hpp"
#include "quartz/client/ui/PageContext.hpp"
#include "quartz/client/ui/PageManager.hpp"

namespace quartz::client::ui
{
    void ShaderEditorPage::render(PageContext& context, PageManager& manager)
    {
        ViewPage legacyPage = ViewPage::ShaderEditor;
        drawShaderEditorPage(context.usb, context.deviceState, context.keyboardInput, context.shaderFramebuffer, context.shaderTransition, context.shaderEditor, legacyPage, context.vertexShaderSource, context.fragmentShaderSource, context.vertexLoadPath, context.fragmentLoadPath, context.settings, context.framebuffer, context.appCpuUsage, context.scrollLockActive, context.capsLockActive);
        if (legacyPage == ViewPage::Main) manager.closeStandalone();
    }
}
