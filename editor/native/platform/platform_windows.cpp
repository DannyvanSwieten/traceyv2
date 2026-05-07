#include "platform.hpp"

// WebView2 + Win32 implementation. Stubbed for now — fill in as a follow-up.
// See plan: WebView2 SDK via NuGet/vcpkg, ICoreWebView2Controller, AddScriptToExecuteOnDocumentCreatedAsync,
// WebMessageReceived, ExecuteScriptAsync, IFileOpenDialog/IFileSaveDialog, GetMessage/DispatchMessage loop.

#error "Windows platform implementation not yet ported. See editor/native/platform/platform_windows.cpp."

namespace tracey_editor {

std::unique_ptr<EditorWindow> create_editor_window() {
    return nullptr;
}

}  // namespace tracey_editor
