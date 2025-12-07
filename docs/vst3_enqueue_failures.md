# VST3 enqueue failure causes

The GUI would surface a "Failed to enqueue VST3 plug-in load" message only when `requestTrackVstLoad` returns `false`. In the current codebase that happens for just two reasons:

- **Invalid request inputs**: a non-positive `trackId` or an empty plug-in path short-circuit the request before anything is queued.【F:src/core/audio_engine.cpp†L217-L236】
- **No matching track/host**: if `trackEnsureVstHost` cannot find the track ID, the load request is rejected and nothing is enqueued.【F:src/core/track_type_vst.cpp†L12-L44】【F:src/core/audio_engine.cpp†L222-L236】

Once a valid host is returned, the request is always enqueued without waiting for completion. Any later failures (e.g., a bad plug-in binary) are reported by the host itself, not by `requestTrackVstLoad`.

## Where GUI freezes or crashes can surface during VST loads

- **GUI request validation and path handling**: `promptAndLoadVstPlugin` falls back to a default plug-in path when the picker is cancelled and rejects the request entirely when the resulting path is empty. A missing file also logs before any work is queued, so the user may see no load progress if the path never resolves to a valid plug-in.【F:src/hosting/VSTGuiThread.cpp†L615-L663】
- **Host acquisition failures**: `trackEnsureVstHost` has to materialize a host for the selected track; if the track ID is not found, the GUI reports that no host is available and the load exits early.【F:src/core/track_type_vst.cpp†L12-L44】【F:src/hosting/VSTGuiThread.cpp†L648-L664】
- **Asynchronous loader/command pipeline issues**: The GUI posts work to the async loader rather than waiting on `requestTrackVstLoad`. If COM initialization fails, the host expires, or the loader defers a queued request, the callback reports failure and the editor never opens.【F:src/hosting/VST3AsyncLoader.cpp†L80-L155】【F:src/hosting/VST3Host.cpp†L753-L788】
- **Host load sequence failures**: Even after the loader schedules the work, `VST3Host::load` can fail while resolving the bundle path, instantiating components/controllers, wiring connection points, or running initialization. Any failure in those steps marks the load as unsuccessful and prevents the editor from opening, which can look like the app froze mid-load.【F:src/hosting/VST3Host.cpp†L875-L1116】
