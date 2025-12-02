# Project roadmap

## VST3 hosting focus
- Maintain compatibility with plug-ins built against modern Steinberg SDK releases while providing shims for older behaviors.
- Keep documentation and host implementations aligned with the currently integrated VST3 SDK.

## Periodic idle callbacks for VST3 views
Plug-ins compiled against older VST3 headers may still expect `IPlugView::onIdle`, even though the method was removed from the official SDK. The project currently vendors the VST3 SDK at version **3.8.0**, which no longer declares `onIdle`, so the host must offer an equivalent timer-driven mechanism.

Planned work:
- Add a host-managed idle service that attaches a periodic timer to each active view, invoking plug-in idle handlers (when present) at a steady cadence suitable for UI housekeeping.
- Provide a 3.7-compatible shim that routes the timer into any optional `IPlugView::onIdle` implementation exposed by the plug-in, with clear fallbacks when the interface is absent.
- Integrate the timer lifecycle with view creation and destruction so callbacks stop promptly when a plug-in UI is closed.
- Document the idle timing policy (default interval, pause behavior while suspended) and make it configurable per host instance if needed.
- Validate the shim with representative plug-ins known to require `onIdle`, confirming no regressions for plug-ins built solely against SDK 3.8.0 interfaces.
