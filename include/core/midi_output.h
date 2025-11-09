#pragma once

void midiOutputSendNoteOn(int portId, int channel, int note, int velocity);
void midiOutputSendNoteOff(int portId, int channel, int note, int velocity);
