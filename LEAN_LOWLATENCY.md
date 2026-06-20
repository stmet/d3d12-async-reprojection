# Lean Ultra-Low-Latency Branch — Analysis, Min-Max Info Dump & TODO

> Branch `lean-lowlatency`. Single purpose: the **lowest-latency, leanest** async-reprojection
> presenter possible. Everything that doesn't directly serve "warp the freshest real frame by
> late-latched mouse input and present it at refresh, NOW" is cruft and gets cut.

---

## 0. The one architectural decision that changes everything

**Turn the game's FSR Frame Generation OFF; let OUR reprojection be the frame generator.**

Right now the project runs *with* FSR3 FG, which forces a huge amount of machinery and fights us:
- FG **interpolates** (holds the newest real frame, shows an in-between) → adds ~1 frame of latency, the opposite of our goal.
- FG has its **own pacing** that our injected delays/presents fight (the "didn't recover" stall).
- FG is the only reason we hook `ffxConfigure`/`ffxDispatch`, capture HUDLessColor, and guess real-vs-interpolated presents.

If FG is **off** (FSR *upscaling* stays on for resolution), then:
- The game presents **real frames only** at its native rate (~60–90 fps) → no interpolated-present bypass guesswork.
- **Our presenter does the frame multiplication** via reprojection to refresh (e.g. 180 Hz) — which is the entire point of the project, done with *extrapolation* (low latency) instead of FFX's *interpolation* (high latency).
- We can **delete every upscaler/FG hook** and all the capture plumbing they feed.

This is the leanest, lowest-latency configuration and it removes ~60% of the codebase.

---

## 1. The essential pipeline (what survives)

```
GAME (FSR upscaling on, FG OFF, ~60-90 fps)
  └─ renders into our replacement backbuffer m_buffers[i]
  └─ Present() → proxy: SubmitGameFrame(m_buffers[i]) → presenter ring; return immediately

PRESENTER THREAD (QPC-paced to vblank, ~180 Hz)
  loop:
    1. sleep until (predicted vblank - leadMs)
    2. pick the FRESHEST GPU-complete replacement buffer (history ring + game fence)
    3. late-latch raw mouse delta (NowQpc)
    4. mode-4 perspective warp of THAT buffer directly into the real backbuffer  ← no copies
    5. (optional) overlay
    6. CallRealPresent
```

**Modules kept:** `common/logger`, `proxy/dxgi_proxy`, `swapchain/proxy_swapchain`,
`present/presenter`, `input/mouse_tracker`, `warp/warp_renderer` (mode-4 only),
`overlay/overlay` (trimmed), and a **gutted** `hooks/hook_manager` (swapchain-create + Present only).

---

## 2. Cruft to strip (and why)

| Module / feature | Action | Why |
|---|---|---|
| `export/export_manager.*` | **delete** | Color copy + triple-slot ring + deferred publish exist only to pair color with hud-less/depth/MV. Lean warps the replacement buffer **directly** → none of it is needed. |
| Depth / MV capture (`CopyDepthMVOnDispatch`, corner readback) | **delete** | Used only by warp modes 2/3 and auto-gain. Per-frame `CopyResource` on the **game's** command list = GPU work that slows the game's own frame → *increases* latency. |
| HUD-less compositor (difference mask, Stage 1/2a) | **delete** | Deferred. Plain warp already "looks fine even at high gain." Racy, never worth it without riding FFX. |
| Auto-gain calibration (`FeedCalibration`, `ReadCornerMV`) | **delete** | The wander (0.27↔0.32/s) is a micro-rubberband source. Replace with **manual gain** (one slider, set once). |
| Warp modes 0 / 2 / 3, `mvFactor` | **delete** | Mode 4 (perspective rotational) is the proven one and needs no depth/MV. |
| All upscaler/FG hooks (NVNGX, FSR2, FSR3 dispatch, `ffxConfigure`, `ffxDispatch`, FG swapchain) | **delete** | Only needed to feed depth/MV/hud-less and to coexist with FG. With FG off, gone. (Keep a thin FG-swapchain stand-down *only if* we later support FG-on.) |
| `hooks/rt_tracker.*` (Track A) | **delete** | Failed, env-gated experiment. |
| `hooks/upscaler_defs.h` | **delete** | Only the upscaler hooks use it. |
| `tracker/resource_tracker.*` | **delete** | Only tracked destruction of intercepted depth/MV/hud-less. |
| `adaptiveDelay` (Anti-Lag POC) | **shelve** | Fights FG pacing; revisit only after the lean base is solid and (if ever) with FG off. |
| LoadLibrary hooks | **review** | Only needed to late-hook upscaler DLLs. With no upscaler hooks, likely deletable. |

---

## 3. Latency min-max opportunities (the meat)

Ordered by expected impact.

1. **Warp the freshest replacement buffer directly (no export copy).**
   Today the (plain-warp) path samples the *deferred export color*, which is 1–2 frames old. Warping
   `m_buffers[freshest-complete]` directly removes that staleness **and** the copy-queue work. Biggest
   single `gameAge` win. *Verify the replacement buffers are SRV-readable (created `ALLOW_RENDER_TARGET`,
   not `DENY_SHADER_RESOURCE` — they are) and that the history-ring depth keeps us off a buffer the game
   is rewriting.*

2. **FG off → no interpolation latency.** Removes ~1 frame inherent to FFX FG and all pacing conflict.

3. **Manual, fixed gain (kill auto-cal wander).** Steady warp magnitude = no breathing/micro-rubberband.

4. **Remove per-frame depth/MV copy from the game's command list.** That copy serializes on the game
   timeline; dropping it lets the game finish frames sooner → fresher capture.

5. **Tight present path.** One mode-4 PSO, minimal barriers, no descriptor churn, no mode branching,
   no `GetDebugCapture` lock. Less CPU between latch and present = lower `inputAge`.

6. **Keep auto-lead** (drives `leadMs` to the knee) — it's already minimizing latch→photon; keep it,
   maybe lower its floor (currently 0.3 ms).

7. **Frame selection: prefer the single freshest complete frame, smallest safe ring** (bufferCount 2–3).
   Smaller ring = lower worst-case age. Re-evaluate `maxFramesInFlight` now that there's no export
   deferral — it may be unnecessary or settable to 1 cleanly.

8. **Submit-relative warp base (already done)** — keep; it's what made motion smooth.

## 4. Performance / leanness opportunities

- Delete the dedicated **copy queue** + 3× allocators + readback buffer (export_manager) → less VRAM,
  fewer submits, fewer fences per frame.
- Drop the **depth/MV/hud-less textures** (3 slots each) → significant VRAM + barrier reduction.
- Fewer hooks = less per-call detour overhead on hot paths (`ffxDispatch` fired multiple times/frame).
- Smaller DLL, faster init, fewer failure modes / TDR surfaces.
- Compile-time: fewer TUs.

## 5. Risks / open questions

- **FOV for mode 4** currently comes from the FSR dispatch capture. With hooks gone, add a **manual FOV
  slider** (or read the game config); mode 4 already falls back to ~59° if missing.
- **Weapon-lock** (mode 4) needs depth. Ultra-lean drops depth → no weapon-lock (gun warps slightly with
  the world). Acceptable for the POC; revisit if needed (could re-add a depth-only capture later).
- **FG-on coexistence** is dropped. If we ever want FG *and* lean, we re-add a thin stand-down path only.
- Confirm the game tolerates FG off + our proxy presenting (it should — simpler than FG-on).
- Confirm replacement-buffer direct-SRV across the flip with `ALLOW_SIMULTANEOUS_ACCESS` if needed.

---

## 6. TODO (phased — "make it happen")

### Phase A — Strip to the lean core  ✅ (built clean; DLL 662KB→611KB)
- [x] A1. Lean `hook_manager`: detours only CreateSwapChain(+ForHwnd); Present extracted (not detoured); all upscaler/FG/LoadLibrary hooks removed.
- [x] A2. Deleted `export/`, `tracker/`, `hooks/rt_tracker.*`, `hooks/upscaler_defs.h`; dropped from CMake.
- [x] A4. Presenter warps the **freshest complete replacement buffer directly** via mode-4 ReprojectInto (null depth/mv/hud, manual FOV); removed export/GetDebugCapture, mvFactor, auto-gain feed.
- [x] A5. Proxy: dropped `ExportManager::OnPresent`; async Present = `SubmitGameFrame` only (FG off ⇒ no bypass).
- [x] A6. Overlay: removed export/rt_tracker debug-capture panel; defaults flipped (mode=4, manual gain). *(Dead UI for removed features still present — cosmetic cleanup pending.)*
- [x] A7. Built clean; deployed. Mode-4 perspective warp of the freshest frame, no capture copies.
- [~] A3 (partial). `FeedCalibration` removed + manual FOV added, but the shader still contains modes 0/2/3 + hud-less branches (unused at mode 4). Cosmetic strip pending; functionally lean (mode 4 with null depth/mv/hud ⇒ pure perspective warp, weapon-lock auto-off).
- [ ] A8. Shelve/remove the `adaptiveDelay` (dc) controller — still present, default off.

### Phase B — Latency validation & tuning
- [ ] B1. Re-add the `RT:` telemetry (mode, gain, present/game fps, inputAge, gameAge, jitter, lead).
- [ ] B2. Measure `gameAge`/`inputAge` vs the old build; confirm the direct-warp drop in `gameAge`.
- [ ] B3. Tune ring size / `maxFramesInFlight`; push `leadMs` floor; confirm no missed-vblank climb.
- [ ] B4. Add manual FOV; verify edge tracking under fast turns.

### Phase C — Quality refinements (only if needed)
- [ ] C1. Optional depth-only capture to restore mode-4 weapon-lock.
- [ ] C2. Revisit adaptive frame pacing **with FG off** (no pacing conflict) if content latency still matters.
- [ ] C3. Disocclusion edge handling for fast flicks (inpaint/clamp), if artifacts bother.

### Phase D — Decide on HUD later (separate effort)
- [ ] D1. If a screen-locked HUD is wanted, evaluate riding FFX UI composition (accepts FG's latency) vs. a synced same-frame capture — out of scope for the lean POC.
