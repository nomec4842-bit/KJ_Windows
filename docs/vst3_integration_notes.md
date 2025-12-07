# Integrating VST3 hosting into KJ

This guide outlines how to wire the existing VST3 host into KJ in a way that mirrors production DAWs (FL Studio, Reaper, Ableton). It focuses on where the current code already provides hosting hooks and what needs to be connected in the engine and GUI.

## Host lifecycle
- **Per-track host**: Each track can lazily materialize a `VST3Host` when needed via `trackEnsureVstHost`, which also associates the track ID for later callbacks. Ensure any feature that needs a plug-in calls this helper so the host exists before issuing commands.【F:src/core/track_type_vst.cpp†L12-L45】
- **Command thread loading**: Plug-in load/unload requests are pushed onto the VST command queue with `requestTrackVstLoad`/`requestTrackVstUnload`. A dedicated worker thread initializes COM, performs the load/unload on the host, and publishes reset notifications so audio and GUI state refresh together.【F:src/core/audio_engine.cpp†L217-L324】
- **Robust load completion**: `VST3Host::load` resolves the bundle path, validates factory support, instantiates component/controller pairs, and marks GUI attach readiness as soon as loading finishes. Hook UI flows to `isPluginReady()` / `waitUntilReady()` to mirror DAW behavior where the editor can open immediately after load.【F:src/hosting/VST3Host.cpp†L827-L937】

## Editor hosting
- **View selection**: The host requests the plug-in’s preferred editor type (defaulting to `kEditor`) and verifies the returned view supports an HWND platform before proceeding. Use this helper when offering alternate view types (e.g., generic editors) so fallbacks remain consistent.【F:src/hosting/VST3Host.cpp†L1521-L1601】
- **Window parenting**: The Windows-only editor stack (`VSTEditorWindow`, `VSTGuiThread`) already creates child windows and attaches the plug-in view. When embedding into new UI containers, reuse `openEditor`/`asyncLoadPluginEditor` so parenting, sizing, and message pumping match the expected VST3 flow.【F:src/hosting/VST3Host.cpp†L1505-L1535】【F:src/hosting/VSTGuiThread.cpp†L240-L666】

## Audio and transport hookup
- **Processing bridge**: `VST3Host::process` clears output buffers, queues parameter and event changes, and dispatches to the plug-in processor. Tie this into the engine’s render callbacks so VST processing happens inside the existing audio graph like in other DAWs.【F:src/hosting/VST3Host.cpp†L1603-L1680】
- **Transport state**: The host exposes `setTransportState` and maintains host flags/position info for the plug-in. Populate this from the engine’s timeline/playback controller (tempo, time signature, position) to give plug-ins accurate transport context similar to Ableton/Reaper behavior.【F:src/hosting/VST3Host.cpp†L1339-L1381】

## Recommended integration steps
1. **Wire track actions**: Route GUI menu items or shortcuts for “Load VST3” / “Unload” to `requestTrackVstLoad` and `requestTrackVstUnload` so all lifecycle logic runs on the worker thread.
2. **Expose editor launch**: Add a UI command that calls `VST3Host::openEditor` (or `asyncLoadPluginEditor` if the plug-in may still be loading) using the track’s host pointer. Reuse the existing safe-parent window creation to ensure focus/move/resize behaves like other DAWs.【F:src/hosting/VSTGuiThread.cpp†L240-L666】
3. **Sync transport**: On every transport state change, forward the updated `HostTransportState` to `setTransportState` before processing the next audio block to keep plug-in timing in sync.【F:src/hosting/VST3Host.cpp†L1339-L1381】
4. **Handle teardown**: When removing a plug-in or track, call `requestTrackVstUnload` and ensure `destroyPluginUI` is invoked so embedded views are detached before the host is destroyed.【F:src/core/audio_engine.cpp†L240-L324】【F:src/hosting/VST3Host.cpp†L1505-L1517】

Following these hooks aligns KJ’s VST3 hosting with mainstream DAWs: asynchronous loading to avoid UI stalls, verified editor embedding on Win32, consistent transport delivery, and clean teardown to prevent orphaned views or stale audio state.
