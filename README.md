# KJ_Windows

## Building on Windows

1. Open a **x64** Developer Command Prompt for Visual Studio 2019 or newer.
2. Use a fresh build directory per architecture (for example, `build/x64`) to avoid mixing 32-bit and 64-bit artifacts.
3. Configure and build with CMake:

   ```bat
   cmake -G "Visual Studio 16 2019" -A x64 -S %cd% -B build\x64
   cmake --build build\x64 --config Release
   ```

If you see `base.lib : fatal error LNK1136: invalid or corrupt file`, the build directory likely contains stale artifacts from a different architecture. Delete the build folder (for example, `rmdir /S /Q build`) and reconfigure using a dedicated directory for the current architecture.

## Third-party attributions

- [Cockos WDL LICE](https://www.cockos.com/wdl/): Lightweight Image Compositing Engine used for GUI rendering. Source files from the official Cockos repository are included in `external/wdl`, redistributed under the terms of the Cockos WDL license.
- Copyright (C) 2005 and later Cockos Incorporated
    
    Portions copyright other contributors, see each source file for more information

    
