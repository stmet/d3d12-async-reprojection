# AsyncReprojection

An in-process **async reprojection / frame-extrapolation** layer for Direct3D 12 games, shipped as a
proxy `dxgi.dll`. Instead of interpolating between two rendered frames (like FSR 3 Frame Generation,
which adds latency), it runs a dedicated presenter thread that **re-warps the latest real frame with
freshly late-latched mouse input and presents at the display's refresh rate** — frame multiplication
by *extrapolation*, so motion gets smoother **and** the camera stays low-latency.

> **Status: experimental proof-of-concept.** The latency/reprojection core is validated; the HUD is a
> known open problem (see [Limitations](#limitations)). Currently targeted at **FSR 3** titles and
> developed/tested against **Cyberpunk 2077** on an **AMD RX 6700 XT**.

---

## How it works

```
GAME (FSR upscaling on, Frame Generation OFF, ~30-130 fps)
  └─ renders into off-screen replacement backbuffers handed out by the proxy
  └─ Present() → proxy hands the finished buffer to the presenter and returns immediately

PRESENTER THREAD (paced to vblank, e.g. 180 Hz)
  loop:
    1. high-res-timer sleep until just before the predicted vblank
    2. pick the freshest GPU-complete replacement buffer
    3. late-latch the raw mouse delta (now)
    4. mode-4 perspective warp of that buffer into the real backbuffer
    5. draw the overlay, then Present
```

- **Proxy swapchain** — the injected `dxgi.dll` detours `CreateSwapChain(ForHwnd)`, hands the game
  off-screen replacement buffers, and owns the real swapchain's presents on its own queue.
- **Mode-4 perspective warp** — a depth-independent rotational reprojection: it reconstructs each
  pixel's view ray, rotates it by the fresh mouse delta, and reprojects. Fold-free, needs no
  motion-vector or depth capture.
- **Angular gain model** — mouse counts → camera yaw/pitch in degrees, so the on-screen magnitude
  scales correctly with FOV automatically. One sensitivity constant, calibrated once.
- **Late-warp pacing** — the mouse is latched at the last possible moment before the vblank, driving
  input→scanout down to sub-millisecond on the camera path.

## Requirements

- A **Direct3D 12** game using **FSR 3** with **Frame Generation OFF** (FSR upscaling on is fine).
- Windows 10 1803+ (uses a high-resolution waitable timer for pacing).
- A flip-model swapchain (all modern D3D12 titles).

## Build

```sh
cmake -S . -B build
cmake --build build --config Release --target dxgi
```

Output: `build/Release/dxgi.dll`. Dependencies (Dear ImGui, Microsoft Detours) are vendored under
`external/`.

## Install

Copy `dxgi.dll` next to the game executable (DLL-proxy load). For Cyberpunk 2077:

```
build/Release/dxgi.dll  →  <Cyberpunk 2077>/bin/x64/dxgi.dll
```

Launch the game. A small `AsyncReproj` badge confirms it loaded; press **INSERT** for the tuning menu.

> Escape hatches: set `ASYNCREPROJ_ASYNC=0` to fall back to a synchronous present-time warp, or
> `ASYNCREPROJ_NO_PROXY=1` to disable the proxy entirely (game runs untouched).

## Usage & tuning

In the overlay (INSERT):

1. **In-game: turn FSR Frame Generation OFF** (FSR upscaling on). This build *is* the frame generator.
2. Enable the warp, set **mode = Perspective rotational**.
3. Set **vertical FOV** to match the game's current FOV.
4. Calibrate **sensitivity (deg/1000 counts)** once: aim at a sharp vertical edge, flick left/right,
   and adjust until the edge holds still under the crosshair. It won't need changing per FOV/scene.
5. Leave **auto-trim**, **auto-lead**, and **auto-suppress in menus** on. Drop the **lead floor** to
   chase latency; raise it if *missed vblanks* climb.

## Telemetry & analysis

The presenter writes `dxgi_telemetry.csv` next to the DLL — one row per ~1 s window plus an event row
on each setting change. Analyze a session with:

```powershell
./analyze_telemetry.ps1            # reads the deployed game copy by default
./analyze_telemetry.ps1 -Path .\dxgi_telemetry.csv
```

It splits the log into config **segments** (so settings A/B directly) and **gates the latency numbers
on present stability** — input→scanout is only a real photon latency when presents are phase-locked to
the refresh, so a segment that isn't is flagged `SUSPECT` instead of reporting a meaningless number.

## Limitations

- **HUD** — the HUD/UI is part of the warped frame, so it swims slightly with the camera. A clean
  screen-locked HUD is an open problem (a difference-mask compositor proved too racy). Menus are
  auto-suppressed (warp off) via cursor-clip detection, but in-gameplay HUD still warps.
- **GPU headroom** — reprojection can only *add* frames if the GPU has spare time. If the game is
  GPU-saturated (e.g. 30 fps with heavy ray tracing using 100% of the GPU), present can't reach the
  refresh rate — that's a physical limit, the same for any frame-generation technique. With headroom
  (game ~60-130 fps) it locks near the refresh at sub-ms camera latency.
- **Scope** — FSR 3 titles only for now; developed against Cyberpunk 2077 / AMD. Other games, GPUs,
  and upscalers are untested.

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) — overlay UI (MIT).
- [Microsoft Detours](https://github.com/microsoft/Detours) — API hooking (MIT).

Inspired by VR asynchronous-reprojection compositors and the AMD FidelityFX / OptiScaler frame-gen
swapchain model.

## License

MIT — see [LICENSE](LICENSE).
