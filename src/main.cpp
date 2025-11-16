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
}

int main() {
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
