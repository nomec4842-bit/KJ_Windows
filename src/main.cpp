#include <windows.h>
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

int main() {
    MessageBoxW(NULL, L"KJ started.", L"DEBUG", MB_OK);
    initTracks();
    initSequencer();
    initAudio();
    MessageBoxW(NULL, L"Audio initialized.", L"DEBUG", MB_OK);
    initGUI();
    MessageBoxW(NULL, L"GUI initialized.", L"DEBUG", MB_OK);
    shutdownAudio();
    return 0;
}
