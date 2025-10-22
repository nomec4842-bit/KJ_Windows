#include <windows.h>
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

int main() {
    MessageBox(NULL, "KJ started.", "DEBUG", MB_OK);
    initTracks();
    initSequencer();
    initAudio();
    MessageBox(NULL, "Audio initialized.", "DEBUG", MB_OK);
    initGUI();
    MessageBox(NULL, "GUI initialized.", "DEBUG", MB_OK);
    shutdownAudio();
    return 0;
}
