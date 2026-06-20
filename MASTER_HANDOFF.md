# AsyncReprojection — Master Handoff & Architecture Guide

> **Purpose of this document:** A comprehensive, single-source-of-truth description of this project. It outlines the project's goals, how the in-process pipeline works, what was tried for HUD extraction, the current FSR 3.1 SDK v2 implementation details, the core mathematical failure modes of difference-based HUD composition under motion, and concrete future paths for the next developer/LLM.
>
> This document consolidates and supersedes `HUD_AND_FG_HANDOFF.md`, `ASYNC_FG_PLAN.md`, and `PIPELINE_ARCHITECTURE.md`.

---

## 1. Project Overview & Core Goals

The goal of this project is to implement **in-process asynchronous reprojection frame generation** for D3D12 games (specifically targeting **Cyberpunk 2077** running with **AMD FSR 3.1 Quality upscaling**). 

By decoupling presentation from the game's rendering thread and executing a high-priority, late-latched camera warp on a presenter thread, we can generate intermediate frames with near-zero input latency.

### Core Architecture Goals
1. **Decoupled input latency:** Read raw mouse deltas on the presenter thread at the last possible millisecond before vblank, warping the most recent GPU-completed frame.
2. **Paced presentation:** Deliver frames exactly at the display's refresh rate (e.g., 144Hz or 180Hz) to eliminate micro-stutter and frame pacing judder.
3. **Clean HUD/UI Handling:** Keep the 2D HUD (text, mini-map, health bars, dialogue boxes) screen-locked and sharp, preventing it from swimming or shearing when the 3D world is warped.
4. **VR Weapon-Locking:** Lock the first-person viewmodel (weapon and attached sights/optics) to the screen so it moves as a solid unit with the player's camera, preventing the gun from tearing away from the center of view.

---

## 2. In-Process Pipeline Architecture

The proxy runs an injected `dxgi.dll` that replaces the DXGI swapchain interface. It is activated in async mode by setting the environment variable `ASYNCREPROJ_ASYNC=1`.

```
GAME RENDER THREAD (60 FPS Game Cadence)
  1. Game logic, inputs, & camera updates.
  2. Renders 3D world into replacement buffers (our proxy textures).
  3. FSR 3.1 Upscale Dispatch
     - [HOOKED] Copies Depth and Motion Vectors (MV) on the game queue command list.
     - [HOOKED] Copies HUDLessColor (pre-UI HDR frame) on the game queue command list.
  4. Game renders HUD/UI on top of the replacement backbuffer.
  5. Proxy Present(N) -> OnPresent()
     - Verify if this was a real present (dispatched) or FSR3-interpolated present.
     - If Interpolated Present -> Bypass (return false, no pacing update, no index advance).
     - If Real Present -> Copy backbuffer (full frame with HUD) to direct slot color texture.
     - Signal game-fence, block game thread if CPU is too far ahead of GPU (limit cap).
     - Push slot index & metadata to Presenter ring.

PRESENTER THREAD (High-Priority, QPC-Paced to Vblank)
  Loop (e.g., 180 Hz Presenter Cadence):
    1. Sleep until predicted next vblank minus `leadMs`.
    2. Late-latch latest raw mouse delta (via QPC-stamped mouse history).
    3. CPU-poll game fence to find the freshest GPU-completed frame in the history ring.
    4. Call `ReprojectInto()` (WarpRenderer):
       - If gUseHudless is enabled, run the difference-based HUD composition shader.
       - Warp the 3D scene (rotational or hybrid MV) using late-latched mouse inputs.
       - Composite the unwarped HUD/UI on top.
    5. Render ImGui Overlay on top of the final backbuffer.
    6. CallRealPresent() to submit the reprojected frame to the display.
```

### Key Subsystems & Proven Features
* **Replacement-Buffer Model:** The game renders into our off-screen textures (`m_buffers[]`). This allows us to intercept the full frame and pacing index before submitting anything to the real swapchain.
* **Reflex-2-Style Pacing:** The presenter wakes up via QPC-accurate sleeps just before vblank (`vblank - leadMs`), late-latches mouse inputs, warps the frame, and presents with minimum latency.
* **Auto-Lead Controller:** Adjusts the wake-up lead time (`leadMs`) to the minimum value that successfully covers the warp GPU render cost, capping at 50% of the refresh interval.
* **Non-Blocking Frame Selection:** Rather than waiting on GPU queues (which causes CPU stalls and misses vblanks), the presenter polls the game fence CPU-side and walks the history ring to select the freshest GPU-complete frame within `bufferCount - 1` depth.
* **Frame-in-Flight Limiter:** Restricts the game's CPU from queuing frames deep. Capping outstanding frames to `maxFramesInFlight = 1` maintains minimal game-frame age (~18ms) and eliminates rubberbanding judder.
* **Interpolated Present Bypassing:** When native FSR3 FG is active, it submits extra presents. We check if `dispatchedThisFrame` is true in `OnPresent`. If false, we skip slot copy, sequence advancement, and pacing telemetry. This anchors pacing to the **true game interval** (~16.6ms at 60fps), stabilizing `autoTrim` and preventing sawtooth snaps in `mvFactor`.

---

## 3. The HUD Extraction & Composition History

### Track A: RTV-Tracking HUDless Capture (Attempted & Failed)
We implemented a system (`rt_tracker.cpp`) to hook `OMSetRenderTargets` and `CreateRenderTargetView` to intercept render targets. We hoped to capture the display-resolution scene right before the UI was drawn.
* **Result:** Cyberpunk 2077 composites the main HUD into the Typeless display-res buffer before any isolable RTV bind occurs. The crosshair is drawn directly onto the final backbuffer. 
* **Conclusion:** RTV-tracking cannot yield a clean HUDless frame in Cyberpunk 2077.

### Track B: FSR 3.1 SDK v2 Resource Interception (Current Path)
By hooking FSR 3.1's backend DLL symbols (`ffxConfigure` and `ffxDispatch`), we capture the native pointers passed by the engine to the FSR3 Frame Generation pipeline.
* **HUDLessColor Resource:** The engine provides FSR3 with a clean, display-resolution, tonemapped scene *without* UI, so FSR3 can interpolate frames without smearing HUD text. We successfully intercept this pointer.
* **Graphics-Queue Dispatch Copy:** Copying the HUDless resource during `OnPresent` using our own command lists caused state mismatches and crashes. We resolved this by performing a `CopyResource` directly inside the `ffxDispatch` hook on the game's own graphics command list, transitioning the resource safely using its registered FSR state.

---

## 4. Current Issues & Mathematical Failure Modes

Despite successfully intercepting the HUDless buffer (`fgHudless`) and aligning it with the full frame (`color`), the difference-based HUD compositor produces visual splitting/doubling under motion, making the game unplayable.

### 1. The Post-Processing Mismatch
Although both buffers represent the same frame, the game applies final post-processing passes (color grading, vignette, film grain, sharpening) **after** the FSR3 dispatch (where `fgHudless` is copied). This creates a 4% to 8% color delta across the entire background.

* **We Tried (Color Normalization):**
  We implemented a clamped per-channel local scale factor in the shader:
  $$\text{scale} = \text{clamp}\left(\frac{\text{fullUnwarped}}{\text{sceneUnwarped} + 0.001}, 0.70, 1.40\right)$$
  This successfully aligned global color grading and exposure differences, reducing background noise to near $0.0$.

### 2. The Spatial Distortion Shift (The Edge-Splitting Blocker)
Post-processing also applies spatial distortions like **lens distortion** and **chromatic aberration** to the final frame. These effects distort and shift pixels radially, by up to 2 pixels near the screen edges.

* **We Tried ($5\times5$ Neighborhood Point-Sampled Search):**
  We implemented a $5\times5$ point-sampled neighborhood search in the shader to find the closest matched color in `sceneUnwarped` around the current pixel:
  ```hlsl
  for (int dy = -2; dy <= 2; ++dy) {
      for (int dx = -2; dx <= 2; ++dx) {
          // ... evaluate scaled difference with neighbor ...
      }
  }
  ```
  While this resolved minor sub-pixel shifts, it introduced a new fundamental failure mode.

### 3. Core Failure Modes of Difference-Based Extraction under Motion
When difference-based HUD extraction is used in a reprojection pipeline, it suffers from two fatal mathematical flaws under motion:

1. **Disocclusion Mismatch (Jittering/Doubling Halos):**
   When the camera rotates, background objects move. At high-contrast edges or areas of disocclusion, the current pixel's color in `fullUnwarped` (current frame) does not exist *anywhere* in the neighborhood of `sceneUnwarped`.
   * The neighborhood search fails to find a match.
   * `minDiffVal` spikes, forcing `alpha = 1.0` (classified as UI).
   * This forces the shader to output the **unwarped** pixel for that edge.
   * The interior of the object warps (`alpha = 0`), but the edge stays unwarped (`alpha = 1`). This splits the object apart, creating severe doubling and decoupling halos.
2. **Fast-Motion Noise:**
   Under rapid motion, the spatial discrepancy between frames exceeds the $5\times5$ (2-pixel) search radius. Large patches of the background fail to align, getting misclassified as UI. This causes massive portions of the screen to flicker between warped and unwarped states, producing high-frequency stuttering and "drunk-vision."

---

## 5. Potential Future Paths & Recommendations

To make the game playable in async mode, the difference-based HUD extraction must be replaced with a robust layer separation pipeline.

### Path A: Ride/Override FFX Frame Generation (Recommended)
Since native FSR3 Frame Generation is active, FFX creates its own swapchain and handles pacing and interpolation. Our proxy swapchain currently stands down (`m_async = false`) when this occurs to prevent `DEVICE_REMOVED`.

Instead of running our own presentation thread, we should **ride the FFX swapchain** and intercept the generated frames:
1. **Intercept the FFX Interpolation Dispatch:**
   In the `ffxFsr3DispatchFrameGeneration` or `ffxDispatch` hook for frame generation, FFX generates the interpolated frame using its own optical-flow shader.
2. **Neuter/Replace the Interpolated Frame:**
   Instead of letting FFX perform optical-flow interpolation (which introduces latency and artifacting), we can override the output of that dispatch. Since we have depth, MVs, and late-latched inputs, we can execute our own reprojection shader directly onto the FFX-allocated interpolated frame resource.
   * This will keep the FFX swapchain and pacing while letting us replace FFX's slow interpolation with our low-latency async reprojection.
3. **Use FFX's Registered UI (If Available):**
   FFX hooks the UI resource via `ffxRegisterFrameinterpolationUiResourceDX12`. Check if this resource is populated in memory. If the game registers a clean UI texture, we can composite it directly on top of our warped background, bypassing difference logic entirely.

### Path B: Viewmodel (Weapon) Layer Separation
Currently, the first-person weapon is handled using a depth-cut threshold (`depth > nearDepthCut`). This creates ghosts on the background when the weapon warps, and the scope optics break because they are rendered at world depth.
1. **Trace Viewmodel Draw Calls:**
   Use the resource tracker or hook command list executions to identify the specific draw calls or render targets where the first-person weapon viewmodel pass is drawn.
2. **Extract Weapon Layer:**
   Copy the viewmodel buffer before it is merged into the world, allowing us to warp the world and the gun as two distinct layers.

---

## 6. File & Module Map
* [src/hooks/hook_manager.cpp](file:///c:/Users/user/Desktop/asyncreprojection_copy/src/hooks/hook_manager.cpp): Contains detours for FSR 3.1 (`ffxConfigure`, `ffxDispatch`) and the resource tracking hooks.
* [src/export/export_manager.cpp](file:///c:/Users/user/Desktop/asyncreprojection_copy/src/export/export_manager.cpp): Handles graphics-queue copying of Depth, MV, and HUDless buffers into the shared triple-buffered slots.
* [src/warp/warp_renderer.cpp](file:///c:/Users/user/Desktop/asyncreprojection_copy/src/warp/warp_renderer.cpp): Contains `kReprojectShader` (the late-latch perspective reprojection and HUD difference composition shader).
* [src/present/presenter.cpp](file:///c:/Users/user/Desktop/asyncreprojection_copy/src/present/presenter.cpp): The high-priority presenter thread executing pacing sleeps, frame history selection, and present calls.
* [src/swapchain/proxy_swapchain.cpp](file:///c:/Users/user/Desktop/asyncreprojection_copy/src/swapchain/proxy_swapchain.cpp): Manages replacement buffers and forwards presents to the presenter in async mode.
