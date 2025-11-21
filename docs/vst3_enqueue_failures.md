# VST3 enqueue failure causes

The GUI prints "Failed to enqueue VST3 plug-in load" when `requestTrackVstLoad` returns `false`. The call can fail for several reasons:

- **Invalid request inputs**: a non-positive `trackId` or an empty plug-in path short-circuit the request before anything is queued.【F:src/core/audio_engine.cpp†L206-L236】
- **No matching track/host**: if `trackEnsureVstHost` cannot find the track ID, or the VST host cannot be constructed, the load request is rejected.【F:src/core/track_type_vst.cpp†L12-L42】【F:src/core/audio_engine.cpp†L206-L236】
- **Command thread timeout**: the GUI waits up to 15 seconds for the load to complete; if the VST command thread does not respond within that window (for example, if the thread is stuck or the host never signals completion), the request fails.【F:src/core/audio_engine.cpp†L218-L236】
- **Host load failure**: the VST worker thread dequeues the command and calls `VST3Host::load`; if that call returns `false` (e.g., missing/invalid plug-in binary), the completion future resolves to failure and the GUI reports the enqueue error.【F:src/core/audio_engine.cpp†L270-L307】
- **Missing/invalid plug-in path**: the GUI falls back to a default VST path when the file picker is cancelled; if that path is empty, the request is never attempted. It also logs when the selected path does not exist, which can later cause the host load step to fail.【F:src/gui/gui_main.cpp†L6620-L6659】

Use these checks to triage why a VST3 plug-in load is being rejected before or during the enqueue process.
