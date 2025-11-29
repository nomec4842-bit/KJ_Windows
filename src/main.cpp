#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

namespace {
void logStartupEvent(const wchar_t* message) {
    OutputDebugStringW((std::wstring(L"[KJ] ") + message + L"\n").c_str());
}

void configureProcessDpiAwareness()
{
#ifdef _WIN32
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto user32 = ::GetModuleHandleW(L"user32.dll");
    auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return;

    using SetProcessDPIAwareFn = BOOL(WINAPI*)();
    auto setAware = reinterpret_cast<SetProcessDPIAwareFn>(::GetProcAddress(user32, "SetProcessDPIAware"));
    if (setAware)
        setAware();
#endif
}
}

int main() {
    configureProcessDpiAwareness();
    logStartupEvent(L"KJ started.");
    initTracks();
    initSequencer();
    initAudio();
    logStartupEvent(L"Audio initialized.");
    initGUI();
    logStartupEvent(L"GUI initialized.");
    shutdownAudio();
    return 0;
}
