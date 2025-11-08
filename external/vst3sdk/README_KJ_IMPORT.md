# VST3 SDK import for KJ

This directory contains a curated subset of the official Steinberg VST3 SDK. Only the folders required for building KJ's VST3
host utilities offline are mirrored here.

## Included modules

- `pluginterfaces/` – public C++ interface headers that define the VST3 contracts consumed by KJ's host code.
- `base/` (including `base/source/` and `base/thread/`) – Steinberg's foundational runtime utilities that back the interfaces and
  hosting helpers (reference counting, threading, smart pointers, etc.).
- `public.sdk/source/vst/hosting/` – the core host framework used to discover, load, and communicate with VST3 plug-ins.
- `public.sdk/source/vst/utility/` – shared helper classes used by the hosting layer (string conversion, memory streams, system
  clipboard access, etc.). The former `public.sdk/source/common/` sources now live here under `utility/common/` so we can keep
  a single utility tree.

## Excluded upstream content

Everything else from the upstream repository was removed to keep the vendored dependency lightweight:

- Developer samples, tutorials, validators, and automated tests – they are not needed for the runtime host dependency.
- Platform wrapper projects (AU, AAX, InterAppAudio, etc.) – KJ is a dedicated VST3 host and does not ship those plug-in types.
- The `vstgui4/` UI toolkit and other binary packaging assets – the host integration currently exposes no UI and we want the
  import to stay source-only.

Refer to `LICENSE.txt` in this directory for the Steinberg licensing terms.
