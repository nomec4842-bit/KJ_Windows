# VST3 enqueue failure causes

The GUI prints "Failed to enqueue VST3 plug-in load" when `requestTrackVstLoad` returns `false`. The call can fail for several reasons:

- **Invalid request inputs**: a non-positive `trackId` or an empty plug-in path short-circuit the request before anything is queued.【F:src/core/audio_engine.cpp†L206-L236】
- **No matching track/host**: if `trackEnsureVstHost` cannot find the track ID, or the VST host cannot be constructed, the load request is rejected.【F:src/core/track_type_vst.cpp†L12-L42】【F:src/core/audio_engine.cpp†L206-L236】
- **Command thread timeout**: the GUI waits up to 15 seconds for the load to complete; if the VST command thread does not respond within that window (for example, if the thread is stuck or the host never signals completion), the request fails.【F:src/core/audio_engine.cpp†L218-L236】
- **Host load failure**: the VST worker thread dequeues the command and calls `VST3Host::load`; if that call returns `false` (e.g., missing/invalid plug-in binary), the completion future resolves to failure and the GUI reports the enqueue error.【F:src/core/audio_engine.cpp†L270-L307】
- **Missing/invalid plug-in path**: the GUI falls back to a default VST path when the file picker is cancelled; if that path is empty, the request is never attempted. It also logs when the selected path does not exist, which can later cause the host load step to fail.【F:src/hosting/VSTGuiThread.cpp†L615-L646】

Use these checks to triage why a VST3 plug-in load is being rejected before or during the enqueue process.

## Where GUI freezes or crashes can surface during VST loads

- **GUI request validation and path handling**: `promptAndLoadVstPlugin` drops a cancelled picker back to the default plug-in path and rejects the request entirely when the resulting path is empty. A missing file also logs before enqueue, so the user may see no load progress if the path never resolves to a valid plug-in.【F:src/hosting/VSTGuiThread.cpp†L615-L663】
- **Host acquisition failures**: `trackEnsureVstHost` has to materialize a host for the selected track; if the track ID is not found or the host cannot be created, the GUI reports that no host is available and the load exits early.【F:src/core/track_type_vst.cpp†L12-L44】【F:src/hosting/VSTGuiThread.cpp†L648-L664】
- **Command-thread bottlenecks/timeouts**: Load/unload work is queued to the VST command thread. If the queue never drains or the thread stalls, the GUI-side futures can time out after 15 seconds, surfacing as failed loads/unloads and apparent UI hangs while the user waits for completion.【F:src/core/audio_engine.cpp†L217-L307】
- **Host load sequence failures**: Even after the request is enqueued, `VST3Host::load` can fail while resolving the bundle path, instantiating components/controllers, wiring connection points, or running initialization. Any failure in those steps marks the load as unsuccessful and prevents the editor from opening, which can look like the app froze mid-load.【F:src/hosting/VST3Host.cpp†L827-L1116】
- **Documented enqueue rejection points**: The GUI strings the above checkpoints into the "Failed to enqueue VST3 plug-in load" message, so these locations are prime suspects when users report freezes/crashes during load attempts.【F:docs/vst3_enqueue_failures.md†L1-L17】
