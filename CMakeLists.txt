cmake_minimum_required(VERSION 3.23)

project(QuartzClient
    VERSION 0.1.0
    LANGUAGES C CXX ASM
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(OpenGL_GL_PREFERENCE GLVND)

set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
set(LIBHAT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL "" FORCE)
set(LIBHAT_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(LIBHAT_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
set(LIBHAT_HINT_X86_64 ON CACHE BOOL "" FORCE)

add_subdirectory(protocol)
add_subdirectory(vendor/glad)
add_subdirectory(vendor/glfw)
add_subdirectory(vendor/libusb)
add_subdirectory(vendor/zydis)
add_subdirectory(vendor/libhat)

add_library(quickjs STATIC
        vendor/quickjs/quickjs.c
        vendor/quickjs/dtoa.c
        vendor/quickjs/libregexp.c
        vendor/quickjs/libunicode.c
        vendor/quickjs/cutils.c
)
target_include_directories(quickjs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION=\"2026-06-04\")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quickjs PRIVATE -fwrapv)
endif()
if(UNIX)
    target_link_libraries(quickjs PUBLIC m)
endif()

file(GLOB_RECURSE QUARTZ_CLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE QUARTZ_CLIENT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

set(QUARTZ_THEME_ASSETS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/QuartzThemeAssets.S")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GENERATE OUTPUT "${QUARTZ_THEME_ASSETS_SOURCE}" CONTENT
".section .rodata.quartz_theme_deviluke,\"a\",@progbits
.balign 16
.global quartz_theme_deviluke_start
.global quartz_theme_deviluke_end
.hidden quartz_theme_deviluke_start
.hidden quartz_theme_deviluke_end
quartz_theme_deviluke_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/deviluke.png\"
quartz_theme_deviluke_end:

.section .rodata.quartz_theme_kurosaki,\"a\",@progbits
.balign 16
.global quartz_theme_kurosaki_start
.global quartz_theme_kurosaki_end
.hidden quartz_theme_kurosaki_start
.hidden quartz_theme_kurosaki_end
quartz_theme_kurosaki_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kurosaki.png\"
quartz_theme_kurosaki_end:

.section .rodata.quartz_theme_yami,\"a\",@progbits
.balign 16
.global quartz_theme_yami_start
.global quartz_theme_yami_end
.hidden quartz_theme_yami_start
.hidden quartz_theme_yami_end
quartz_theme_yami_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/yami.png\"
quartz_theme_yami_end:

.section .rodata.quartz_theme_kirisaki,\"a\",@progbits
.balign 16
.global quartz_theme_kirisaki_start
.global quartz_theme_kirisaki_end
.hidden quartz_theme_kirisaki_start
.hidden quartz_theme_kirisaki_end
quartz_theme_kirisaki_start:
.incbin \"${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kirisaki.png\"
quartz_theme_kirisaki_end:

.section .note.GNU-stack,\"\",@progbits
")
set_source_files_properties("${QUARTZ_THEME_ASSETS_SOURCE}" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/deviluke.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kurosaki.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/yami.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/silhouettes/kirisaki.png")

# Keep a tiny, checked patch layer over the vendored editor instead of carrying a fork.
# 1) Completion selection only scrolls on keyboard navigation, so the wheel can freely browse the popup.
# 2) Programmatic SetText refreshes preserve viewport + cursors/selections. Live/read-only views such as
#    disassembly/state must not jump to line 1, and editable hot-reloaded sources must not move the caret to 0:0.
#    Undo history and diagnostic overlays still reset because SetText semantically replaces the document.
# 3) Hover popups follow the pointer continuously, keep a sane width, close when the pointer leaves, and
#    don't reopen every frame for tokens whose callback rejected the hover. Context menus always take
#    priority over hover popups. Editors also claim wheel input while hovered so reaching a code-view edge
#    cannot unexpectedly scroll the containing Quartz page. Read-only editors leave Ctrl+Click available
#    to their host instead of treating it as a multi-caret gesture.
# 4) Quartz overlays can query exact document screen positions using the same origin/glyph geometry that
#    the editor itself uses, avoiding approximations that break under scrolling or DPI scaling.
# 5) Hosts can intercept a mouse click while the editor is processing it. This is intentionally before
#    selection/context handling so read-only code views can implement navigation without racing Render().
set(QUARTZ_TEXT_EDITOR_HEADER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/TextEditor.h")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit/TextEditor.h" QUARTZ_TEXT_EDITOR_HEADER_CONTENT)
set(QUARTZ_EDITOR_SCREEN_API_OLD "\tinline float GetLineHeight() const { return glyphSize.y; }\n\tinline float GetGlyphWidth() const { return glyphSize.x; }")
set(QUARTZ_EDITOR_SCREEN_API_NEW "\tinline float GetLineHeight() const { return glyphSize.y; }\n\tinline float GetGlyphWidth() const { return glyphSize.x; }\n\tinline ImVec2 GetDocPosScreenPos(DocPos pos) const { auto vis = docPos2VisPos(normalizePos(pos)); return ImVec2(cursorScreenPos.x + textLeftOffset + vis.column * glyphSize.x, cursorScreenPos.y + vis.row * glyphSize.y); }\n\tinline ImVec2 GetTextAreaScreenMin() const { return ImVec2(cursorScreenPos.x + textLeftOffset, cursorScreenPos.y); }\n\tinline ImVec2 GetTextAreaScreenMax() const { return ImVec2(cursorScreenPos.x + textRightOffset, cursorScreenPos.y + textSize.y); }")
string(FIND "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}" "${QUARTZ_EDITOR_SCREEN_API_OLD}" QUARTZ_EDITOR_SCREEN_API_POS)
if(QUARTZ_EDITOR_SCREEN_API_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit screen-coordinate API patch no longer matches the vendored header")
endif()
string(REPLACE "${QUARTZ_EDITOR_SCREEN_API_OLD}" "${QUARTZ_EDITOR_SCREEN_API_NEW}" QUARTZ_TEXT_EDITOR_HEADER_CONTENT "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}")
set(QUARTZ_EDITOR_CLICK_API_OLD "\tstruct PopupData {\n\t\tDocPos pos;\n\t\tvoid* userData;\n\t};")
set(QUARTZ_EDITOR_CLICK_API_NEW "\tstruct PopupData {\n\t\tDocPos pos;\n\t\tvoid* userData;\n\t};\n\n\tstruct MouseClickData {\n\t\tDocPos pos;\n\t\tvoid* userData;\n\t\tImGuiMouseButton button;\n\t\tbool ctrl;\n\t\tbool shift;\n\t\tbool alt;\n\t};\n\n\tinline void SetMouseClickCallback(std::function<bool(MouseClickData& data)> callback) { mouseClickCallback = callback; }\n\tinline void ClearMouseClickCallback() { mouseClickCallback = nullptr; }\n\tinline bool HasMouseClickCallback() const { return mouseClickCallback != nullptr; }")
string(FIND "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}" "${QUARTZ_EDITOR_CLICK_API_OLD}" QUARTZ_EDITOR_CLICK_API_POS)
if(QUARTZ_EDITOR_CLICK_API_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit mouse-click API patch no longer matches the vendored header")
endif()
string(REPLACE "${QUARTZ_EDITOR_CLICK_API_OLD}" "${QUARTZ_EDITOR_CLICK_API_NEW}" QUARTZ_TEXT_EDITOR_HEADER_CONTENT "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}")
set(QUARTZ_EDITOR_CLICK_MEMBER_OLD "\tstd::function<void(PopupData& data)> textContextMenuCallback;\n\tstd::function<void(PopupData& data)> textHoverCallback;\n\tImVec2 popupWindowPos;")
set(QUARTZ_EDITOR_CLICK_MEMBER_NEW "\tstd::function<void(PopupData& data)> textContextMenuCallback;\n\tstd::function<void(PopupData& data)> textHoverCallback;\n\tstd::function<bool(MouseClickData& data)> mouseClickCallback;\n\tImVec2 popupWindowPos;")
string(FIND "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}" "${QUARTZ_EDITOR_CLICK_MEMBER_OLD}" QUARTZ_EDITOR_CLICK_MEMBER_POS)
if(QUARTZ_EDITOR_CLICK_MEMBER_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit mouse-click member patch no longer matches the vendored header")
endif()
string(REPLACE "${QUARTZ_EDITOR_CLICK_MEMBER_OLD}" "${QUARTZ_EDITOR_CLICK_MEMBER_NEW}" QUARTZ_TEXT_EDITOR_HEADER_CONTENT "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}")
file(WRITE "${QUARTZ_TEXT_EDITOR_HEADER_SOURCE}" "${QUARTZ_TEXT_EDITOR_HEADER_CONTENT}")

set(QUARTZ_TEXT_EDITOR_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/TextEditor.cpp")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit/TextEditor.cpp" QUARTZ_TEXT_EDITOR_CONTENT)
set(QUARTZ_AUTOCOMPLETE_SCROLL_OLD "if (selected) {\n\t\t\t\t\t\tImGui::SetScrollHereY(1.0f);\n\t\t\t\t\t}")
set(QUARTZ_AUTOCOMPLETE_SCROLL_NEW "if (selected && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {\n\t\t\t\t\t\tImGui::SetScrollHereY(0.5f);\n\t\t\t\t\t}")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_AUTOCOMPLETE_SCROLL_OLD}" QUARTZ_AUTOCOMPLETE_SCROLL_POS)
if(QUARTZ_AUTOCOMPLETE_SCROLL_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit autocomplete scroll patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_AUTOCOMPLETE_SCROLL_OLD}" "${QUARTZ_AUTOCOMPLETE_SCROLL_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_SETTEXT_STATE_OLD "void TextEditor::setText(const std::string_view& text) {\n\t// load text into document and reset overlays\n\tdocument.setText(config, text);\n\ttransactions.reset();\n\tcursors.clearAll();\n\tclearMarkers();\n\tclearSquiggles();\n\tresetScrolling();\n}")
set(QUARTZ_SETTEXT_STATE_NEW "void TextEditor::setText(const std::string_view& text) {\n\t// Quartz: replacing generated/live text must not destroy the user's viewport or caret.\n\tconst auto previousCursors = cursors;\n\tdocument.setText(config, text);\n\ttransactions.reset();\n\tcursors = previousCursors;\n\tfor (auto& cursor : cursors) {\n\t\tcursor.update(normalizePos(cursor.getInteractiveStart()), normalizePos(cursor.getInteractiveEnd()));\n\t}\n\tclearMarkers();\n\tclearSquiggles();\n\t// Cancel stale deferred cursor/line scrolling without resetting Dear ImGui's current scroll position.\n\tensureVisiblePos = DocPos(invalidLine, 0);\n\tscrollToLineNumber = invalidLine;\n}")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_SETTEXT_STATE_OLD}" QUARTZ_SETTEXT_STATE_POS)
if(QUARTZ_SETTEXT_STATE_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit SetText state patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_SETTEXT_STATE_OLD}" "${QUARTZ_SETTEXT_STATE_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_STALE_ROW_OLD "\tauto& row = at(pos.row);\n\tauto& line = document[row.line];\n\n\tDocPos docPos;")
set(QUARTZ_STALE_ROW_NEW "\tauto& row = at(pos.row);\n\t// The row cache can still describe the previous document after SetText() or a\n\t// large deletion. Clamp before indexing the newly shortened document.\n\tif (row.line >= document.size()) {\n\t\treturn document.getBottom();\n\t}\n\tauto& line = document[row.line];\n\n\tDocPos docPos;")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_STALE_ROW_OLD}" QUARTZ_STALE_ROW_POS)
if(QUARTZ_STALE_ROW_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit stale row bounds patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_STALE_ROW_OLD}" "${QUARTZ_STALE_ROW_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_EDITOR_WHEEL_OLD "if (ImGui::BeginChild(title, size, childFlags, windowFlags)) {\n\t\t// perform last part of scrolling reset here (as it requires the correct Dear ImGui context)")
set(QUARTZ_EDITOR_WHEEL_NEW "if (ImGui::BeginChild(title, size, childFlags, windowFlags)) {\n\t\t// Keep wheel input inside the editor instead of letting an exhausted child scroll its parent page.\n\t\tif (ImGui::IsWindowHovered() && !autocomplete.isActive()) {\n\t\t\tauto window = ImGui::GetCurrentWindow();\n\t\t\tImGui::SetKeyOwner(ImGuiKey_MouseWheelY, window->ID, ImGuiInputFlags_LockThisFrame);\n\t\t\tImGui::SetKeyOwner(ImGuiKey_MouseWheelX, window->ID, ImGuiInputFlags_LockThisFrame);\n\t\t}\n\n\t\t// perform last part of scrolling reset here (as it requires the correct Dear ImGui context)")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_EDITOR_WHEEL_OLD}" QUARTZ_EDITOR_WHEEL_POS)
if(QUARTZ_EDITOR_WHEEL_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit wheel ownership patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_EDITOR_WHEEL_OLD}" "${QUARTZ_EDITOR_WHEEL_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_READONLY_CTRL_CLICK_OLD "\t\t\t\tauto addCursor = ImGui::GetIO().ConfigMacOSXBehaviors\n\t\t\t\t\t? ImGui::IsKeyDown(ImGuiMod_Alt) :\n\t\t\t\t\tImGui::IsKeyDown(ImGuiMod_Ctrl);")
set(QUARTZ_READONLY_CTRL_CLICK_NEW "\t\t\t\tauto addCursor = !config.readOnly && (ImGui::GetIO().ConfigMacOSXBehaviors\n\t\t\t\t\t? ImGui::IsKeyDown(ImGuiMod_Alt) :\n\t\t\t\t\tImGui::IsKeyDown(ImGuiMod_Ctrl));")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_READONLY_CTRL_CLICK_OLD}" QUARTZ_READONLY_CTRL_CLICK_POS)
if(QUARTZ_READONLY_CTRL_CLICK_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit read-only Ctrl+Click patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_READONLY_CTRL_CLICK_OLD}" "${QUARTZ_READONLY_CTRL_CLICK_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_HOVER_MOUSE_SETUP_OLD "void TextEditor::handleMouseInteractions() {\n\tauto io = ImGui::GetIO();\n\tauto mousePos = ImGui::GetMousePos() - cursorScreenPos;\n\tauto absoluteMousePos = ImGui::GetMousePos() - ImGui::GetWindowPos();")
set(QUARTZ_HOVER_MOUSE_SETUP_NEW "void TextEditor::handleMouseInteractions() {\n\tauto io = ImGui::GetIO();\n\tauto mousePos = ImGui::GetMousePos() - cursorScreenPos;\n\tauto absoluteMousePos = ImGui::GetMousePos() - ImGui::GetWindowPos();\n\tconst bool contextPopupOpen = ImGui::IsPopupOpen(\"TextContextMenu\") || ImGui::IsPopupOpen(\"LineNumberContextMenu\");\n\tconst ImGuiHoveredFlags editorHoverFlags = ImGui::IsPopupOpen(\"TextHoverPopup\") ? ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem : ImGuiHoveredFlags_None;\n\tif (textHoverCallback && (contextPopupOpen || !ImGui::IsWindowHovered(editorHoverFlags))) popupDocPos = DocPos(invalidLine, 0);")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_HOVER_MOUSE_SETUP_OLD}" QUARTZ_HOVER_MOUSE_SETUP_POS)
if(QUARTZ_HOVER_MOUSE_SETUP_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit hover mouse setup patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_HOVER_MOUSE_SETUP_OLD}" "${QUARTZ_HOVER_MOUSE_SETUP_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_EDITOR_CLICK_OLD "\ttypeSetter.screenPos2DocPos(\n\t\tdocument,\n\t\tImVec2((mousePos.x - textLeftOffset) / glyphSize.x, mousePos.y / glyphSize.y),\n\t\tglyphPos,\n\t\tcursorPos);\n\n\tpanning &= config.panMode && ImGui::IsMouseDown(ImGuiMouseButton_Middle);")
set(QUARTZ_EDITOR_CLICK_NEW "\ttypeSetter.screenPos2DocPos(\n\t\tdocument,\n\t\tImVec2((mousePos.x - textLeftOffset) / glyphSize.x, mousePos.y / glyphSize.y),\n\t\tglyphPos,\n\t\tcursorPos);\n\n\t// Quartz: offer clicks to the host before selection/multicaret processing. Use text-area geometry\n\t// rather than IsWindowHovered so a mouse-transparent hover popup cannot hide the underlying editor.\n\tbool hostHandledClick = false;\n\tif (overText && mouseClickCallback) {\n\t\tint clickedButton = -1;\n\t\tif (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clickedButton = ImGuiMouseButton_Left;\n\t\telse if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) clickedButton = ImGuiMouseButton_Right;\n\t\telse if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) clickedButton = ImGuiMouseButton_Middle;\n\t\tif (clickedButton >= 0) {\n\t\t\tMouseClickData data{glyphPos, document.getUserData(glyphPos.line), static_cast<ImGuiMouseButton>(clickedButton), io.KeyCtrl, io.KeyShift, io.KeyAlt};\n\t\t\thostHandledClick = mouseClickCallback(data);\n\t\t}\n\t}\n\tbool forcedTextContextClick = false;\n\tif (!hostHandledClick && overText && textContextMenuCallback && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {\n\t\tpopupDocPos = glyphPos;\n\t\tImGui::OpenPopup(\"TextContextMenu\");\n\t\tforcedTextContextClick = true;\n\t}\n\n\tpanning &= config.panMode && ImGui::IsMouseDown(ImGuiMouseButton_Middle);")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_EDITOR_CLICK_OLD}" QUARTZ_EDITOR_CLICK_POS)
if(QUARTZ_EDITOR_CLICK_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit host mouse-click patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_EDITOR_CLICK_OLD}" "${QUARTZ_EDITOR_CLICK_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_HOVER_WINDOW_OLD "\t} else if (ImGui::IsWindowHovered()) {\n\t\t// show text cursor if required")
set(QUARTZ_HOVER_WINDOW_NEW "\t} else if (ImGui::IsWindowHovered(editorHoverFlags)) {\n\t\t// show text cursor if required")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_HOVER_WINDOW_OLD}" QUARTZ_HOVER_WINDOW_POS)
if(QUARTZ_HOVER_WINDOW_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit hover window patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_HOVER_WINDOW_OLD}" "${QUARTZ_HOVER_WINDOW_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_EDITOR_CLICK_DISPATCH_OLD "\t\t} else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {\n\t\t\t// handle right clicks by setting up context menu (if required)")
set(QUARTZ_EDITOR_CLICK_DISPATCH_NEW "\t\t} else if (!forcedTextContextClick && !hostHandledClick && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {\n\t\t\t// handle right clicks by setting up context menu (if required)")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_EDITOR_CLICK_DISPATCH_OLD}" QUARTZ_EDITOR_CLICK_DISPATCH_POS)
if(QUARTZ_EDITOR_CLICK_DISPATCH_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit right-click dispatch patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_EDITOR_CLICK_DISPATCH_OLD}" "${QUARTZ_EDITOR_CLICK_DISPATCH_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_EDITOR_LEFT_CLICK_OLD "\t\t} else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {\n\t\t\t// handle left mouse button actions")
set(QUARTZ_EDITOR_LEFT_CLICK_NEW "\t\t} else if (!hostHandledClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {\n\t\t\t// handle left mouse button actions")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_EDITOR_LEFT_CLICK_OLD}" QUARTZ_EDITOR_LEFT_CLICK_POS)
if(QUARTZ_EDITOR_LEFT_CLICK_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit left-click dispatch patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_EDITOR_LEFT_CLICK_OLD}" "${QUARTZ_EDITOR_LEFT_CLICK_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_HOVER_TRACK_OLD "\t\t} else if (textHoverCallback && IsMousePosOverGlyph(ImGui::GetMousePos())) {\n\t\t\t// capture position for text hover popup\n\t\t\tpopupDocPos = document.findWordStart(glyphPos, true);\n\t\t\tauto vizPos = docPos2VisPos(popupDocPos);\n\n\t\t\tpopupWindowPos = ImVec2(\n\t\t\t\tvizPos.column * glyphSize.x + textLeftOffset + cursorScreenPos.x,\n\t\t\t\tvizPos.row * glyphSize.y + cursorScreenPos.y);\n\n\t\t\t// only open popup if required\n\t\t\tif (!ImGui::IsPopupOpen(\"TextHoverPopup\")) {\n\t\t\t\tImGui::OpenPopup(\"TextHoverPopup\");\n\t\t\t}\n\t\t}")
set(QUARTZ_HOVER_TRACK_NEW "\t\t} else if (!hostHandledClick && !forcedTextContextClick && !contextPopupOpen && textHoverCallback && IsMousePosOverGlyph(ImGui::GetMousePos())) {\n\t\t\t// Follow the pointer smoothly. If a callback rejects this token by closing the popup, don't reopen it\n\t\t\t// every frame until the hovered word changes.\n\t\t\tconst auto hoverPos = document.findWordStart(glyphPos, true);\n\t\t\tconst bool hoverChanged = hoverPos != popupDocPos;\n\t\t\tpopupDocPos = hoverPos;\n\t\t\tpopupWindowPos = ImGui::GetMousePos() + ImVec2(16.0f * fontScaleDpi, 18.0f * fontScaleDpi);\n\t\t\tif (hoverChanged && !ImGui::IsPopupOpen(\"TextHoverPopup\")) ImGui::OpenPopup(\"TextHoverPopup\");\n\t\t} else if (textHoverCallback && (hostHandledClick || forcedTextContextClick || contextPopupOpen)) {\n\t\t\tpopupDocPos = DocPos(invalidLine, 0);\n\t\t}")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_HOVER_TRACK_OLD}" QUARTZ_HOVER_TRACK_POS)
if(QUARTZ_HOVER_TRACK_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit hover tracking patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_HOVER_TRACK_OLD}" "${QUARTZ_HOVER_TRACK_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
set(QUARTZ_HOVER_POPUP_OLD "\tif (ImGui::IsPopupOpen(\"TextHoverPopup\")) {\n\t\tImGui::SetNextWindowPos(popupWindowPos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));\n\n\t\tif (ImGui::BeginPopup(\"TextHoverPopup\", ImGuiWindowFlags_NoFocusOnAppearing)) {\n\t\t\tif (textHoverCallback) {\n\t\t\t\tImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());\n\t\t\t\ttextHoverCallback(popupData);\n\n\t\t\t} else {\n\t\t\t\tImGui::CloseCurrentPopup();\n\t\t\t}\n\n\t\t\tImGui::EndPopup();\n\t\t}\n\t}")
set(QUARTZ_HOVER_POPUP_NEW "\tif (!ImGui::IsPopupOpen(\"TextContextMenu\") && !ImGui::IsPopupOpen(\"LineNumberContextMenu\") && ImGui::IsPopupOpen(\"TextHoverPopup\")) {\n\t\tImGui::SetNextWindowPos(popupWindowPos, ImGuiCond_Always);\n\t\tImGui::SetNextWindowSizeConstraints(ImVec2(220.0f * fontScaleDpi, 0.0f), ImVec2(520.0f * fontScaleDpi, FLT_MAX));\n\n\t\tif (ImGui::BeginPopup(\"TextHoverPopup\", ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {\n\t\t\tif (textHoverCallback && popupDocPos.line != invalidLine) {\n\t\t\t\tImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());\n\t\t\t\ttextHoverCallback(popupData);\n\n\t\t\t} else {\n\t\t\t\tImGui::CloseCurrentPopup();\n\t\t\t}\n\n\t\t\tImGui::EndPopup();\n\t\t}\n\t}")
string(FIND "${QUARTZ_TEXT_EDITOR_CONTENT}" "${QUARTZ_HOVER_POPUP_OLD}" QUARTZ_HOVER_POPUP_POS)
if(QUARTZ_HOVER_POPUP_POS EQUAL -1)
    message(FATAL_ERROR "ImGuiColorTextEdit hover popup patch no longer matches the vendored source")
endif()
string(REPLACE "${QUARTZ_HOVER_POPUP_OLD}" "${QUARTZ_HOVER_POPUP_NEW}" QUARTZ_TEXT_EDITOR_CONTENT "${QUARTZ_TEXT_EDITOR_CONTENT}")
file(WRITE "${QUARTZ_TEXT_EDITOR_SOURCE}" "${QUARTZ_TEXT_EDITOR_CONTENT}")

add_executable(QuartzClient
        ${QUARTZ_CLIENT_SOURCES}
        ${QUARTZ_THEME_ASSETS_SOURCE}
        vendor/ImGuiColorTextEdit/TextDiff.cpp
        ${QUARTZ_TEXT_EDITOR_SOURCE}
        vendor/imgui/imgui.cpp
        vendor/imgui/imgui_draw.cpp
        vendor/imgui/imgui_tables.cpp
        vendor/imgui/imgui_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_glfw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_opengl3.cpp
        ${QUARTZ_CLIENT_HEADERS}
)

set_target_properties(QuartzClient PROPERTIES
    OUTPUT_NAME quartz
)

target_include_directories(QuartzClient PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuiColorTextEdit
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
)

target_link_libraries(QuartzClient PRIVATE
        QuartzProtocol
        glad
        glfw
        usb-1.0
        Zydis
        libhat::libhat
        quickjs
)

target_compile_features(QuartzClient PRIVATE cxx_std_20)
