# Async Reprojection — Pipeline & Architecture

In-process `dxgi.dll` proxy that adds **async reprojection / frame-warp** to a D3D12 game
(Cyberpunk 2077, FSR 3.0 upscaling, FG OFF). It presents at the monitor's refresh by re-warping
the game's freshest finished frame with fresh mouse input + depth, decoupling on-screen motion
from the game's render rate.

> Scope: proof-of-concept. Single game/upscaler path (FSR 3.0). Not production-hardened.

---

## 1. Module map

| File | Lines | Role |
|---|---|---|
| `proxy/dxgi_proxy.cpp` | 133 | `DllMain`; exports `CreateDXGIFactory*`; loads the real `dxgi.dll`, returns a proxy factory. |
| `swapchain/proxy_swapchain.cpp` | 179 | Wraps `IDXGISwapChain`; intercepts `CreateSwapChainForHwnd`, owns the replacement backbuffers the game renders into. |
| `hooks/hook_manager.cpp` | 249 | Detours factory/device/swapchain vtables; wires `Present`, spins up the presenter + capture. |
| `present/presenter.cpp` | 687 | **The heart.** Dedicated present thread: frame selection, vblank pacing, mouse latch, calls the warp, presents, telemetry. |
| `warp/warp_renderer.cpp` | 940 | The warp pass: HLSL shader (4 modes), root constants, the camera-latch math. |
| `capture/depth_capture.cpp` | 599 | Late-hooks FSR3 dispatch; copies depth + MV into stable textures; **MV→camera-translation least-squares fit**. |
| `capture/ffx_defs.h` | 148 | Minimal FSR3 dispatch struct layouts (no SDK dependency). |
| `input/mouse_tracker.cpp` | 95 | Raw mouse accumulation + time-indexed lookups with forward velocity extrapolation. |
| `overlay/overlay.cpp` | 700 | ImGui tuning menu (Insert) + always-on watermark. |
| `common/{logger,telemetry}.cpp` | 90/93 | File log + per-config telemetry segments. |

---

## 2. Data flow (one presented frame)

```
Game thread:                          Present thread (presenter.cpp):
  ffxFsr3*DispatchUpscale  ──hook──►  DepthCapture: copy depth+MV on the
    (depth, MV, FOV, near/far)          game's command list (stable textures)
  render into replacement BB                       │
  ExecuteCommandLists / Signal ─────►  1. poll game fence, pick FRESHEST
                                          GPU-complete replacement frame
                                       2. sleep until ~just before next vblank
                                       3. latch mouse @ photon time
                                       4. WarpRenderer::ReprojectInto(frame→real BB)
                                       5. Overlay::RenderOverlay (UI unwarped, on top)
                                       6. real Present(syncInterval=1)
```

Key inversion: the game's own `Present` is **not** driven on its cadence; the presenter owns
presentation and re-uses the newest finished frame as many times as the refresh demands.

---

## 3. The four warp modes (`warp_renderer.cpp` shader)

Internal mode id **==** UI dropdown id (cleaned up; no more "mode 6"):

| id | name | needs depth | what it does |
|---|---|---|---|
| 0 | Rotational shift | no | flat UV translate (`suv = uv + gWarp`); legacy fallback. |
| 1 | Perspective rotational | optional | reconstruct view ray, rotate by fresh mouse delta, reproject. Fold-free, depth-independent. The proven low-latency default. |
| 2 | Perspective + parallax | yes | mode 1 + a depth-correct shift from the fitted **global camera translation**. |
| 3 | True reprojection (raymarch) | yes | march each view ray through the frozen depth buffer to the first hit (real occlusion). |

### Mode 1 core — ray rotate + reproject
```hlsl
float3 d = float3(ndc.x*tw, ndc.y*th, 1.0);          // view ray from FOV
float3 f = Ry(gYaw) * Rx(gPitch) * d;                // rotate by fresh mouse delta
suv = float2((f.x/f.z/tw + 1)*0.5, (1 - f.y/f.z/th)*0.5);
```
This reduces to a uniform shift at screen-center but correctly curves at the edges — a flat UV
shift gets that wrong.

### Weapon-lock (modes 1/2/3 share it) — the dilated near-mask
The first-person gun is camera-locked (near plane → high reversed-Z depth). It must stay put while
the world reprojects. The captured depth is **render-res** (softer than display-res color), so a
single-sample near test lets the gun's silhouette leak → "outline ghost". Fix: dilate the mask.
```hlsl
float dMax = dHere;
// max() over 6 neighbors at radius gMaskDilate → swallow the soft silhouette
bool nearHere = dMax > gNearCut;                 // reversed-Z: gun = HIGH depth
// + "enclosed" test (gWeaponDilate) locks the see-through optic lens (renders at WORLD depth)
if (nearHere) suv = i.uv;                         // screen-lock the gun + its optics
```
Plus a **path-aware source reject**: if a world pixel's gather path crosses the gun, sample
un-warped instead of smearing a gun copy (self-scales with turn speed).

### Mode 3 core — raymarch the frozen depth
```hlsl
float3 ro = camT * reprojScale;                  // current cam pos in frozen frame (small!)
float3 f  = R * viewRay;                          // ray dir in frozen frame
for (k < steps) {                                 // adaptive tMax = (1/d_own)*1.5
    P = ro + f*t;  q = project(P);
    sd = depth(q);  if (sd > nearCut) { skip; }   // gun-skip → no ghost copies
    if (P.z > 1/sd) { binary-refine 6x; hit; }    // refine kills stair-step laddering
}
```
> **Reality check baked into the design:** the benefit of positional reprojection scales with how
> *stale* the frozen frame is. At 67fps→180Hz the correction is small; it shines at ~30fps. Moving
> objects (cars/NPCs) still judder at game rate — camera reprojection can't fix that (that's the
> per-object-MV "soup" we deliberately avoid).

---

## 4. MV-as-sensor: global camera-translation fit (`depth_capture.cpp`)

We get **no view matrix** from FSR — only depth, MV, FOV, near/far. So translation is *estimated*:
the MV field = rotation + translation parallax + object motion. A per-frame GPU compute solves a
**depth-weighted least-squares** for the single 3D camera translation, with two nuisance unknowns
absorbing depth-flat error:

```
flowUV = w·(M·T) + U      // w = 1/Z (inverse depth); T = (Tx,Ty,Tz); U = uniform nuisance flow
```
- Rotation is subtracted first (known from mouse).
- **U absorbs leftover rotation / sens-calib / MV-scale error** (all depth-FLAT) so they can't
  contaminate **T** (depth-VARYING parallax). 5×5 normal equations → CPU Gaussian solve, EMA + soft
  deadzone. Runs on the present queue, read back a few frames latent (no stall).

**Critical gotcha (documented so it never bites again):** CP2077 is reversed-Z **infinite-far** and
passes `cameraNear=16000, cameraFar=0`. The naive `linZ = near·far/(…)` then = 0 → `w=∞` → garbage.
Correct relation is **`Z ∝ 1/d`** (no near/far needed). Used in both the fit and mode 3.

---

## 5. Latency / smoothness features ("input minimizing")

| Feature | Where | Effect |
|---|---|---|
| **Freshest-frame selection** | presenter §1 | walks history newest-first to the newest *GPU-complete* frame → minimum frame age. |
| **Late-warp (Reflex-2 pacing)** | presenter | sleep until just before vblank, then latch+warp+present → minimal input→photon, 1 present/refresh. |
| **Photon-time latch** | warp + presenter | extrapolate the displayed camera to *vblank + mid-screen scanout* (when pixels actually light up), not the latch instant. Deterministic, removes lead+scanout staleness. |
| **Auto-lead controller** | presenter | adapts the pre-vblank wake so missed-vblanks stay ~0 while creeping latency tighter; hard-capped at ½ refresh. |
| **High-res sleep** | presenter `SleepUntilQpc` | waitable timer + short spin → wakes within tens of µs of target. |
| **Mouse forward-extrapolation** | mouse_tracker | present-time latches extrapolate from recent velocity (≤40ms) → no duplicate/stepped frames at low fps. |
| **Angular-gain model** | warp | `yaw = counts·(deg/1000)·π/180`; FOV-correct, DPI cancels — no re-tune on zoom/ADS. |
| **Auto-trim** | presenter | sets the late-latch window to a fraction of a game frame; self-adjusts to fps drift. |

---

## 6. Bottlenecks & limits

1. **Object/animation judder (the ceiling).** Cars, NPCs, FX freeze for one game frame then snap.
   Camera reprojection cannot touch this; only selective per-object MV reprojection could (bounded
   "soup", not yet built). At 30fps this is the dominant remaining choppiness.
2. **Disocclusion.** Reprojection reveals area the frozen frame never rendered. Mode 3 has a
   furthest-neighbor fill; modes 1/2 fall back to un-warped ("stuck band"). Proper push-pull
   inpainting is the next quality lever.
3. **Mode-3 raymarch cost.** Per-pixel march at 180Hz/1440p is the perf risk; `raySteps` trades
   quality for cost. Coarse→binary-refine keeps it affordable; a coarse-prepass would help further.
4. **MV-fit noise.** Strafe-X is weakly observable without near+far depth contrast; vertical (Y) is
   the weakest axis. Acceptable for FPS feel; deadzone + EMA tame standing jitter.
5. **See-through optics.** World-depth glass can't be isolated by depth-lock; mode 3 routes ADS
   through the perspective path (rotation-only) to keep the optic steady.

## 7. Legacy / dead-weight still present
- The root-constant block still carries unused fields from the removed MV "soup" modes
  (`mvScale`, `mvThreshold`, `depthEdge*`, `camRejectK`) and the HUD-mask path. Harmless, but a
  future pass could shrink the 34-constant buffer.
- HUD-less compositor params exist for the planned UI-aware path (not active in the HUDLess flow).

## 8. Build / deploy
```
cmake --build build --config Release --target dxgi
copy build\Release\dxgi.dll  "C:\Games\Cyberpunk 2077\bin\x64\dxgi.dll"
# log: that folder's dxgi.log
```
