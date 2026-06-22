# AsyncReprojection

[![build](https://github.com/stmet/d3d12-async-reprojection/actions/workflows/build.yml/badge.svg)](https://github.com/stmet/d3d12-async-reprojection/actions/workflows/build.yml)

An in-process **async reprojection / frame-extrapolation** layer for Direct3D 12 games, shipped as a
proxy `dxgi.dll`. Instead of interpolating between two rendered frames (like FSR 3 Frame Generation,
which adds latency), it runs a dedicated presenter thread that **re-warps the latest real frame with
freshly late-latched mouse input — and, optionally, the game's depth — then presents at the display's
refresh rate**. Frame multiplication by *extrapolation*, so motion gets smoother **and** the camera
stays low-latency.

It scales from a depth-free rotational warp up to **true depth reprojection** (raymarching the frozen
depth buffer with a camera reconstructed from FOV + mouse + an MV-derived translation estimate).

> **Status: experimental proof-of-concept.** The latency/reprojection core is validated; the HUD is a
> known open problem (see [Limitations](#limitations)). Currently targeted at **FSR 3** titles and
> developed/tested against **Cyberpunk 2077** on an **AMD RX 6700 XT**. See
> [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full pipeline write-up.

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
    3. late-latch the raw mouse delta, extrapolated to photon time
    4. reproject that buffer into the real backbuffer (chosen mode)
    5. draw the overlay (unwarped, on top), then Present
```

- **Proxy swapchain** — the injected `dxgi.dll` detours `CreateSwapChain(ForHwnd)`, hands the game
  off-screen replacement buffers, and owns the real swapchain's presents on its own queue.
- **Depth / MV capture** — late-hooks the FSR3 upscale dispatch and copies the game's depth and
  motion-vector buffers (on the game's own command list) into stable textures the warp can sample.
- **Angular gain model** — mouse counts → camera yaw/pitch in degrees, so the on-screen magnitude
  scales correctly with FOV automatically. One sensitivity constant, calibrated once.
- **Late-warp + photon-time latch** — the mouse is latched at the last moment before vblank and
  extrapolated to when the pixels actually light up, driving input→photon down to sub-millisecond.

### Reprojection modes

| Mode | Needs depth | What it does |
|---|---|---|
| 0 · Rotational shift | no | flat UV translate; legacy fallback. |
| 1 · Perspective rotational | optional | reconstruct each view ray, rotate by the fresh mouse delta, reproject. Fold-free, depth-independent. **Default.** |
| 2 · Perspective + parallax | yes | mode 1 + a depth-correct shift from a **global camera translation** estimated from the MV field. |
| 3 · True reprojection | yes | raymarch each view ray through the frozen depth buffer to the first surface hit — real occlusion + disocclusion fill. Shines at low game fps. |

Modes 1–3 keep the first-person weapon **screen-locked** (depth near-mask) so it doesn't smear while
the world reprojects around it.

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
2. Enable the warp. Start at **mode 1 (Perspective rotational)**.
3. Leave **auto FOV** on (the FSR-captured FOV drives the warp); a manual FOV is the fallback.
4. Calibrate **sensitivity (deg/1000 counts)** once: aim at a sharp vertical edge, flick left/right,
   and adjust until the edge holds still under the crosshair. It won't need changing per FOV/scene.
5. Leave **auto-trim**, **auto-lead**, and **photon-time latch** on. Drop the **lead floor** to chase
   latency; raise it if *missed vblanks* climb.
6. For depth modes: tune the **weapon-lock** (near cut / mask dilate / optic fill) so the gun stays
   clean. Mode 2/3 add a small **parallax / translation scale** — keep it low; it tears if cranked.

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
  screen-locked HUD is an open problem (a difference-mask compositor proved too racy) and is the next
  major piece of work.
- **Moving objects** — camera reprojection smooths *camera* motion; cars/NPCs/animations still update
  at the game's frame rate (per-object reprojection is intentionally out of scope). Most visible when
  the game runs well below the refresh.
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
