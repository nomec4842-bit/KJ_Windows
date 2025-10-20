#include <windows.h>
#include "core/audio_engine.h"
#include "gui/gui_main.h"

int main() {
    MessageBox(NULL, "KJ started.", "DEBUG", MB_OK);
    initAudio();
    MessageBox(NULL, "Audio initialized.", "DEBUG", MB_OK);
    initGUI();
    MessageBox(NULL, "GUI initialized.", "DEBUG", MB_OK);
    shutdownAudio();
    return 0;
}
