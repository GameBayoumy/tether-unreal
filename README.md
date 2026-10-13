# TetherXR

An Unreal Engine plugin that renders **VR Preview** and **`-vr` games** into a Quest headset on macOS, through [Tether](https://github.com/GameBayoumy/tether).

Epic's OpenXR plugin doesn't run on the Mac: it has no Metal render path. TetherXR is a separate head-mounted display built on Unreal's public XR interfaces (XRBase). It talks to Tether through a small C API, so the engine doesn't need patching.

## Requirements

- An Apple silicon Mac on macOS 27, with Tether installed and running
- A Quest connected over USB, with Link started from Tether
- Unreal Engine 5.8 (primary target) or 5.3, using the Metal RHI

## How it works

- **Finding Tether.** Tether keeps a runtime manifest at `~/Library/Application Support/Tether/openxr_runtime.json`. When the plugin loads, it reads `runtime.library_path` from that manifest and `dlopen`s `libTetherOpenXR.dylib`. If the manifest isn't there, it tries `/Applications/Tether.app/Contents/Resources/openxr/libTetherOpenXR.dylib`. All it uses is the `tether_direct_*` API in `TetherXR/Source/ThirdParty/Tether/tether_direct.h`. If the library is missing, no HMD is created and the plugin does nothing.
- **Tracking.** Every game frame the plugin reads Tether's tracking, then reads it again on the render thread as a late update. Tether's space is OpenXR-style (+Y up, −Z forward, metres) and gets converted to Unreal's (X forward, Z up, centimetres via WorldToMeters).
- **Rendering.** Both eyes render side by side into a render target of `2 × eye_width` by `eye_height`, which the plugin allocates itself. Each eye uses Tether's asymmetric field of view and IPD.
- **Present.** A custom present on the stereo window passes the finished texture and the exact head pose it was rendered for (in Tether's space) to `tether_direct_submit`, on Unreal's own Metal command queue. After that the normal present runs, so the Mac window keeps showing the spectator view (by default one eye, cropped to fill).
- **Recentering.** The view recenters when stereo starts, when a Link session starts, when you hold the Meta button (Tether's `recenter_count` changes), and on `ResetOrientationAndPosition`, `ResetOrientation` and `ResetPosition`. The floor and stage origins (default: `LocalFloor` on 5.8, `Floor` on 5.3) sit `floor_height` below the recentred eye level, the same as Tether's OpenXR runtime.
- **Controllers.** MotionControllerComponent sources `Left`/`Right`/`LeftGrip`/`RightGrip` give the grip pose. `LeftAim`/`RightAim` give the aim pose, which is the grip pitched up 15°, as in Tether's runtime.
- **Input.** The plugin sends the engine's Oculus Touch keys, so the VR template's Enhanced Input mappings work unchanged:
  - Triggers and grips: axis, click and trigger touch
  - Thumbsticks: X/Y/2D, click, touch, and up/down/left/right
  - X/Y and A/B: click and touch
  - The left menu button
  - Thumb rests: `OculusTouch_Left_ThumbRest` and `OculusTouch_Right_ThumbRest`. The engine has no thumb-rest keys, so TetherXR registers these itself.

  Input is only sent while stereo is on, so the editor UI never sees it.

## Installing

1. Build the plugin (see below), or use a packaged build.
2. Copy `out/TetherXR-UE<version>` to one of:
   - `YourProject/Plugins/TetherXR`, for a single project
   - `<Engine>/Engine/Plugins/Marketplace/TetherXR`, for every project on that engine
3. The plugin is enabled by default, so you don't need to edit the `.uproject`.

The plugin needs no paths or settings. It finds Tether through the manifest that Tether writes when it launches.

If Epic's OpenXR plugin is also enabled (the VR template enables it), you can leave it on. On the Mac, TetherXR raises its own `[HMDPluginPriority]` to 60, which outranks OpenXRHMD (20), and reports itself as connected whenever Tether is installed, so it wins HMD selection. To override this, set `TetherXR=<priority>` under `[HMDPluginPriority]` in your project's `DefaultEngine.ini`. You can also pick an HMD explicitly with `-hmd=TetherXR`.

## Using it

- **VR Preview.** Start Link from Tether, then choose **Play > VR Preview**. The button is enabled whenever Tether is running.
- **Standalone.** Launch with `-game -vr`.
- **Status.** The `TetherXR.Status` console command logs whether Tether is installed, running and streaming, and the headset's eye size, refresh rate, IPD and FOV.
- **Logs.** Look for the `LogTetherXR` category.

## Building

```sh
scripts/build.sh          # UE 5.8 and 5.3, whichever are installed
scripts/build.sh 5.8      # one version
```

The script runs `RunUAT.sh BuildPlugin -TargetPlatforms=Mac` for each engine and writes `out/TetherXR-UE<version>/`. Each build's log goes to `out/TetherXR-UE<version>.log`. Engines are expected under `/Users/Shared/Epic Games/UE_<version>`. To use a different location, set `UE_ROOT_5_8` or `UE_ROOT_5_3`.

One source tree builds for both versions. The API differences are handled with `ENGINE_MAJOR_VERSION`/`ENGINE_MINOR_VERSION` checks (`TETHERXR_UE_5_8_OR_NEWER`). Only 5.3 and 5.8 are tested.

**UE 5.3 with Xcode 16 or newer.** UE 5.3 targets Xcode 15, and a newer Apple clang rejects a few of its engine headers. For 5.3 builds only, `build.sh` applies these settings through clang's `CCC_OVERRIDE_OPTIONS` hook, leaving the engine install untouched:

- raises the deployment target to macOS 14 (required by the newer libc++)
- silences two header warnings
- turns off access checking (`-fno-access-control`) for one engine template that newer clang rejects

## Limitations

- **Mac and Metal only.** On any other RHI the plugin tracks but sends no frames.
- **Frame rate.** The headset gets frames at the rate the Mac window presents. A 60 Hz display caps the headset at 60 fps.
- **Stereo modes.** Only side-by-side stereo is supported. Mobile multiview is unsupported, and instanced stereo is untested. Set `vr.InstancedStereo=False` in your Mac config.
- **Shadow ensure.** With instanced stereo off, UE 5.8 hits a one-time engine ensure in shadow stencil culling ("Shadow projection rendered to unexpected view"). It's harmless. To silence it, set `r.Shadow.StencilCulling=0`.
- **Not supported:** haptics (accepted and ignored), hand tracking, stereo layers sent as separate headset layers, and passthrough.
- **Thumb-rest key names.** The engine defines no thumb-rest keys, so their names are TetherXR's own.

## License

MIT. See [LICENSE](LICENSE). The headers in `TetherXR/Source/ThirdParty/Tether` come from Tether, which is also MIT. This repository contains no Epic Games source code.
