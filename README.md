# RenderHeight

RenderHeight is an FH2 Mod Platform DLL for Azimuth 1.0.3. It adds one selectable even-numbered height to Azimuth's resolution choices. It does not patch the Azimuth or game executable.

## Repository contents

- `src/render_height.cpp` — mod initialization, settings, signature scan, hook, and resolution-list insertion.
- `scripts/verify_signatures.py` — checks the target executable hash and confirms the two signatures are unique.
- `scripts/ida_annotate.py` — guarded IDAPython annotations for the verified Azimuth 1.0.3 functions.
- `settings.ini.example` — default-off example configuration.
- `artifacts/RenderHeight.dll` — built x64 DLL used in the setup archive.
- `TO RELEASE/RenderHeight-setup.zip` — install-ready files, separate from the source tree.

## How it works

At startup, the DLL scans Azimuth's executable sections for two unique signatures. If both match, it asks the FH2 Mod Platform to hook Azimuth's resolution-list builder and uses Azimuth's own vector insertion helper to add the selected height. The feature is off by default. When enabled, it writes the selected value to `[title-1a96545b] resolution` in `data/azimuth.ini`; it saves the prior value and restores it when disabled, provided the INI still contains the value the mod wrote. A missing or ambiguous signature or a rejected hook disables the feature rather than applying a guessed patch.

The adjustable height range is 360–4320 pixels in steps of 2. The example and release configuration start with `enabled=0` and `height=540`.

## Verification and limits

The signatures were verified against Azimuth 1.0.3, SHA-256 `ebfd5d43a165dcec5d4007b9d41ed5b2ed122fcd02ecfef7e4ed35b40826821f`. A runtime test logged render height 540 and confirmed the hook inserted that choice. Windows reported a 1920×1080 display mode during the test. The D3D12 swap chain/backbuffer was not captured, so this does not prove the dimensions of every game render target or guarantee scene-only scaling.

The test was rolled back: the title resolution is 720, the original fullscreen setting was restored, and the installed configuration is disabled.

## Build

Requirements: CMake 3.20 or later, Ninja, an x86_64 llvm-mingw toolchain, and the FH2 Mod Platform source tree.

From this directory, with llvm-mingw's `bin` on `PATH`:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="C:/path/to/fh2-mod-platform/cmake/mingw-w64-x86_64.cmake" \
  -DFH2MOD_PLATFORM_DIR="C:/path/to/fh2-mod-platform"
cmake --build build --target RenderHeight --config Release
```

The output is `build/RenderHeight.dll`.

## Install and use

1. Close Azimuth.
2. Extract `TO RELEASE/RenderHeight-setup.zip` into the Azimuth installation folder, merging the `fh2-mods` and `RenderHeight` folders.
3. Start FH2 through Azimuth. The mod is installed but disabled by default.
4. Open **Pause → Settings → MODS**, enable **Custom launcher height**, and set **Launcher height (px)** to an even value from 360 to 4320. Accept the restart prompt.
5. Check `fh2-mods/platform.log` for the signature and hook messages. On restart, Azimuth should log the selected render height. The tested value was 540.

## Uninstall

1. In **Pause → Settings → MODS**, turn **Custom launcher height** off and accept the restart prompt. Confirm the prior Azimuth title resolution is restored.
2. Close Azimuth.
3. Remove `fh2-mods/RenderHeight.dll` and the `RenderHeight` data folder beside `azimuth.exe`.

If the mod cannot load to restore the value, restore a backup of `data/azimuth.ini`. Keep a backup before installing or changing settings.
