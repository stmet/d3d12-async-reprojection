#include "overlay.h"
#include "../common/logger.h"
#include "../hooks/hook_manager.h"
#include "../hooks/rt_tracker.h"
#include "../export/export_manager.h"
#include "../input/mouse_tracker.h"
#include "../warp/warp_renderer.h"
#include "../present/presenter.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <detours.h>

// Provided by imgui_impl_win32.cpp — feeds Win32 messages into ImGui.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// ---- Recording ring: independent of swapchain buffer count, just enough depth to keep the
// CPU from stalling on the GPU between presents. ----
constexpr UINT kFrames = 3;

bool                        s_init        = false;
bool                        s_initFailed  = false;   // hard failure — stop retrying
bool                        s_visible     = false;   // full menu toggled by INSERT

ID3D12Device*               s_device      = nullptr;
ID3D12CommandQueue*         s_queue       = nullptr; // present queue (borrowed, AddRef'd)

ID3D12DescriptorHeap*       s_srvHeap     = nullptr; // shader-visible, for ImGui font/textures
ID3D12DescriptorHeap*       s_rtvHeap     = nullptr; // one RTV per backbuffer
DescriptorHeapAllocator     s_srvAlloc;

ID3D12CommandAllocator*     s_alloc[kFrames] = {};
ID3D12GraphicsCommandList*  s_cmdList     = nullptr;
ID3D12Fence*                s_fence       = nullptr;
HANDLE                      s_fenceEvent  = nullptr;
UINT64                      s_fenceVal    = 0;
UINT64                      s_frameFence[kFrames] = {};
UINT                        s_frameIdx    = 0;

UINT                        s_bufferCount = 0;
UINT                        s_rtvIncrement = 0;
DXGI_FORMAT                 s_rtvFormat   = DXGI_FORMAT_UNKNOWN;

HWND                        s_hwnd        = nullptr;
WNDPROC                     s_oWndProc    = nullptr;

// ---- Capture debug views (P1) ----
// Three persistent SRV descriptors in the ImGui SRV heap, re-pointed each frame at the latest
// captured color / depth / MV textures so they can be drawn with ImGui::Image.
bool                        s_dbgAlloc    = false;
bool                        s_showDebug   = false;
D3D12_CPU_DESCRIPTOR_HANDLE s_dbgCpu[6]   = {};   // [3] = hud-less candidate (Track A), [4] = FG hudless, [5] = FG UI
D3D12_GPU_DESCRIPTOR_HANDLE s_dbgGpu[6]   = {};

// ---- Cursor/raw-input neutralization ----
// Cyberpunk (and most mouselook games) clips the OS cursor to the window centre and reads camera
// motion from raw input (WM_INPUT), recentring via SetCursorPos every frame. To make the menu
// usable we Detour-hook ClipCursor/SetCursorPos and turn them into no-ops while the menu is open
// (freeing the cursor), release the active clip on open, and swallow WM_INPUT so the camera stops.
typedef BOOL (WINAPI* PFN_ClipCursor)(const RECT*);
typedef BOOL (WINAPI* PFN_SetCursorPos)(int, int);

PFN_ClipCursor   pfn_ClipCursor          = nullptr;
PFN_SetCursorPos pfn_SetCursorPos        = nullptr;
PFN_SetCursorPos pfn_SetPhysicalCursorPos = nullptr;
bool             s_inputHooks            = false;
RECT             s_gameClip              = {};
bool             s_haveClip              = false;

BOOL WINAPI hkClipCursor(const RECT* r) {
    if (r) { s_gameClip = *r; s_haveClip = true; } else { s_haveClip = false; }
    if (s_visible) return TRUE;                 // keep the cursor free while the menu is open
    return pfn_ClipCursor(r);
}
BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (s_visible) return TRUE;                 // block the game's per-frame recentre while open
    return pfn_SetCursorPos(x, y);
}
BOOL WINAPI hkSetPhysicalCursorPos(int x, int y) {
    if (s_visible) return TRUE;
    return pfn_SetPhysicalCursorPos(x, y);
}

void InstallInputHooks() {
    if (s_inputHooks) return;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    pfn_ClipCursor           = (PFN_ClipCursor)GetProcAddress(u32, "ClipCursor");
    pfn_SetCursorPos         = (PFN_SetCursorPos)GetProcAddress(u32, "SetCursorPos");
    pfn_SetPhysicalCursorPos = (PFN_SetCursorPos)GetProcAddress(u32, "SetPhysicalCursorPos");
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (pfn_ClipCursor)   DetourAttach(&(PVOID&)pfn_ClipCursor, hkClipCursor);
    if (pfn_SetCursorPos) DetourAttach(&(PVOID&)pfn_SetCursorPos, hkSetCursorPos);
    if (pfn_SetPhysicalCursorPos && pfn_SetPhysicalCursorPos != pfn_SetCursorPos)
        DetourAttach(&(PVOID&)pfn_SetPhysicalCursorPos, hkSetPhysicalCursorPos);
    s_inputHooks = (DetourTransactionCommit() == NO_ERROR);
    LOG_INFO("Overlay: cursor/input hooks %s", s_inputHooks ? "installed" : "FAILED");
}

void RemoveInputHooks() {
    if (!s_inputHooks) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (pfn_ClipCursor)   DetourDetach(&(PVOID&)pfn_ClipCursor, hkClipCursor);
    if (pfn_SetCursorPos) DetourDetach(&(PVOID&)pfn_SetCursorPos, hkSetCursorPos);
    if (pfn_SetPhysicalCursorPos && pfn_SetPhysicalCursorPos != pfn_SetCursorPos)
        DetourDetach(&(PVOID&)pfn_SetPhysicalCursorPos, hkSetPhysicalCursorPos);
    DetourTransactionCommit();
    s_inputHooks = false;
}

// Release the cursor clip (menu opening) or restore the game's last clip (menu closing).
void ApplyMenuCursorState() {
    if (s_visible) {
        if (pfn_ClipCursor) pfn_ClipCursor(nullptr); else ::ClipCursor(nullptr);
    } else if (s_haveClip) {
        if (pfn_ClipCursor) pfn_ClipCursor(&s_gameClip); else ::ClipCursor(&s_gameClip);
    }
}

// ---- ImGui DX12 backend SRV descriptor callbacks (1.92 dynamic-texture path) ----
void SrvAllocFn(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    s_srvAlloc.Alloc(cpu, gpu);
}
void SrvFreeFn(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    s_srvAlloc.Free(cpu, gpu);
}

bool IsInputMessage(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
        // WM_SETCURSOR is handled explicitly in HookedWndProc (not swallowed generically) so we
        // can force a visible arrow while the menu is open and let the game re-hide it when closed.
        default:
            return false;
    }
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Toggle the menu on INSERT keydown. Done before forwarding so it works regardless of focus state.
    if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
        s_visible = !s_visible;
        ApplyMenuCursorState(); // free the cursor on open / restore the game's clip on close
    }

    // Own the cursor while the menu is open: force a single visible OS arrow and block the game's
    // own WM_SETCURSOR (which would hide it). When the menu is closed we fall through to the game,
    // which restores/hides its cursor on the next WM_SETCURSOR. This avoids the double-cursor and
    // the "cursor stays after closing" issues.
    if (msg == WM_SETCURSOR && s_init && s_visible) {
        ::SetCursor(::LoadCursorA(nullptr, IDC_ARROW));
        return TRUE;
    }

    // Tap the raw mouse stream for the warp's async late-latch, regardless of menu state, before
    // it's forwarded to (or swallowed from) the game.
    if (msg == WM_INPUT)
        MouseTracker::OnRawInput(lParam);

    if (s_init) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return TRUE;
        if (s_visible) {
            // Swallow the game's raw-input (camera) and standard input while the menu is open.
            // WM_INPUT still goes to DefWindowProc for system cleanup, just never to the game proc.
            if (msg == WM_INPUT)
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            if (IsInputMessage(msg))
                return TRUE;
        }
    }

    return CallWindowProcW(s_oWndProc, hwnd, msg, wParam, lParam);
}

void DrawDebugViews() {
    DebugCapture cap{};
    if (!ExportManager::Instance().GetDebugCapture(cap)) {
        ImGui::TextDisabled("No capture yet — needs an FSR frame to be intercepted.");
        return;
    }

    if (!s_dbgAlloc) {
        for (int i = 0; i < 6; ++i)
            s_srvAlloc.Alloc(&s_dbgCpu[i], &s_dbgGpu[i]);
        s_dbgAlloc = true;
    }

    ImGui::Text("seq %llu   validity 0x%X", (unsigned long long)cap.seq, cap.validity);
    ImGui::TextDisabled("bits: 1=color 2=depth 4=mv 8=upscalerParams");
    ImGui::Text("render %ux%u   mvScale % .5f, % .5f", cap.renderW, cap.renderH, cap.mvScaleX, cap.mvScaleY);
    ImGui::Text("near % .4f   far % .1f   fovV % .3f", cap.camNear, cap.camFar, cap.camFovV);
    ImGui::Separator();

    auto drawTex = [&](int idx, ID3D12Resource* tex, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* label) {
        if (!tex || w == 0 || h == 0) { ImGui::TextDisabled("%s: none", label); return; }
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = fmt;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        s_device->CreateShaderResourceView(tex, &sd, s_dbgCpu[idx]);

        const float dw = 360.0f;
        float dh = dw * (float)h / (float)w;
        ImGui::Text("%s  %ux%u  fmt=%u", label, w, h, (unsigned)fmt);
        ImGui::Image((ImTextureID)s_dbgGpu[idx].ptr, ImVec2(dw, dh));
    };

    drawTex(0, cap.color, cap.colorSrvFmt, cap.colorW, cap.colorH, "Color");
    drawTex(1, cap.depth, cap.depthSrvFmt, cap.depthW, cap.depthH, "Depth (R)");
    drawTex(2, cap.mv,    cap.mvSrvFmt,    cap.mvW,    cap.mvH,    "Motion (RG)");

    // Track A: hud-less candidate (only when ASYNCREPROJ_HUDLESS=1).
    if (RtTracker::Enabled()) {
        ImGui::Separator();
        RtTracker::Stats st = RtTracker::GetStats();
        ImGui::Text("Hud-less: hooks %s   RTV map %u   candidates/frame %u   captures %llu",
                    st.hooksInstalled ? "ON" : "off", st.rtvMapSize, st.candidatesLastFrame,
                    (unsigned long long)st.captures);
        int cp = RtTracker::GetCapturePoint();
        const char* cpNames[] = { "first bind after upscale (scene, pre-HUD)",
                                  "last bind before present (pre-crosshair)",
                                  "last unbind (full HUD)" };
        if (ImGui::Combo("capture point", &cp, cpNames, 3)) RtTracker::SetCapturePoint(cp);
        if (st.lastCandidateW)
            ImGui::Text("last candidate %ux%u fmt=%u", st.lastCandidateW, st.lastCandidateH, st.lastCandidateFmt);
        uint32_t hw = 0, hh = 0; RtTracker::GetHudlessSize(hw, hh);
        drawTex(3, RtTracker::GetHudless(), RtTracker::GetHudlessFormat(), hw, hh, "Hud-less (candidate)");
    }

    if (cap.fgHudless) {
        ImGui::Separator();
        drawTex(4, cap.fgHudless, cap.fgHudlessSrvFmt, cap.fgHudlessW, cap.fgHudlessH, "FG HUD-less Color");
    }
    if (cap.fgUi) {
        ImGui::Separator();
        drawTex(5, cap.fgUi, cap.fgUiSrvFmt, cap.fgUiW, cap.fgUiH, "FG UI Layer");
    }
}

void BuildUI() {
    ImGuiIO& io = ImGui::GetIO();

    // Always-on status badge so the user can confirm the overlay loaded even with the menu hidden.
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGuiWindowFlags badgeFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##areproj_status", nullptr, badgeFlags)) {
        ImGui::Text("AsyncReproj  %.1f FPS", io.Framerate);
        ImGui::TextDisabled("[INSERT] menu");
    }
    ImGui::End();

    if (!s_visible)
        return;

    ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Async Reprojection")) {
        ImGui::Text("Phase P3 - present-time warp");
        ImGui::Text("Application %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Separator();
        WarpParams& wp = WarpRenderer::Params();
        ImGui::Checkbox("Enable warp", &wp.enable);
        int modeIdx = (wp.mode == 4) ? 1 : (wp.mode == 2 ? 2 : (wp.mode == 3 ? 3 : 0));
        if (ImGui::Combo("mode", &modeIdx, "Rotational shift\0Perspective rotational\0Hybrid (corner cancel)\0True reproject (per-pixel MV)\0"))
            wp.mode = (modeIdx == 1) ? 4 : (modeIdx == 2 ? 2 : (modeIdx == 3 ? 3 : 0));
        ImGui::Checkbox("auto-calibrate gain", &wp.autoCalibrate);
        if (wp.autoCalibrate) {
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Regresses the far-corner motion vector (pure camera screen motion)\n"
                                  "against the mouse delta each frame to derive the warp gain.\n"
                                  "Move the camera (mouse) to feed it. Magnitude only — sign stays manual.");
            ImGui::Text("gain %.4f (auto)  measured %.4f  conf %.0f%%  n=%d",
                        wp.gain, wp.calGain, wp.calConfidence * 100.0f, wp.calSamples);
            // Strength trim: auto-cal tends to read a touch strong (the warp's fresh-delta window is
            // wider than one game frame). Lower this if the warp over-shoots your aim.
            ImGui::SliderFloat("auto-gain strength", &wp.calScale, 0.2f, 1.2f, "%.2f");
            if (wp.detectedSign != 0.0f && wp.detectedSign != wp.sign)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   "detected sign %+.0f differs from yours %+.0f — try 'flip sign'",
                                   wp.detectedSign, wp.sign);
        } else {
            ImGui::SliderFloat("gain", &wp.gain, 0.0f, 0.3f, "%.4f");
        }
        ImGui::Checkbox("auto-trim ms", &wp.autoTrim);
        if (wp.autoTrim) {
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drives 'trim ms' from the measured game-frame interval so the warp\n"
                                  "leads by a fixed fraction of one game frame at any fps/refresh.\n"
                                  "Lower 'trim strength' if the warp over-leads (feels too strong).");
            ImGui::Text("trim %.1f ms (auto)", wp.trimMs);
            ImGui::SliderFloat("trim strength", &wp.trimScale, 0.2f, 0.8f, "%.2f");
        } else {
            ImGui::SliderFloat("trim ms", &wp.trimMs, -40.0f, 40.0f, "%.1f");
        }
        if (ImGui::Button("flip sign")) wp.sign = -wp.sign;
        ImGui::SameLine();
        ImGui::Text("sign %+.0f    warp UV %.4f, %.4f", wp.sign, wp.lastU, wp.lastV);

        if (wp.mode == 4) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "perspective rotational (fold-free, depth-independent)");
            ImGui::Checkbox("weapon lock (near-field stays put)", &wp.weaponLock);
            if (wp.weaponLock) {
                ImGui::SameLine(); ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Keeps the gun + its optics/sights screen-locked (VR weapon layer)\n"
                                      "while the world and world-anchored markers reproject around them.");
                ImGui::SliderFloat("near cut (gun depth)", &wp.nearDepthCut, 0.0f, 1.0f, "%.3f");
                ImGui::SliderFloat("optic fill radius", &wp.weaponDilate, 0.0f, 0.15f, "%.3f");
                ImGui::SameLine(); ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fills holes inside the gun (the scope lens renders at world depth).\n"
                                      "Raise to match the lens size; 0 = off.");
            }
        }
        if (wp.mode == 2 || wp.mode == 3) {
            ImGui::SliderFloat("MV scale", &wp.mvScale, 0.0f, 2.0f, "%.3f");
            ImGui::Text("MV factor %.2f (game-frames ahead)", wp.lastMvFactor);
        }
        if (wp.mode == 2) {
            ImGui::SliderFloat("obj threshold", &wp.mvThreshold, 0.0f, 0.02f, "%.4f");
            ImGui::SliderFloat("cam reject", &wp.camRejectK, 0.0f, 4.0f, "%.2f");
            ImGui::SliderFloat("near cut (gun)", &wp.nearDepthCut, 0.0f, 1.0f, "%.3f");
            ImGui::Checkbox("depth-edge guard", &wp.depthEdge);
            ImGui::SameLine();
            ImGui::SliderFloat("edge thr", &wp.depthEdgeThresh, 0.0f, 0.05f, "%.4f");
        }

        if (wp.mode >= 2) {
            ImGui::Spacing();
            ImGui::Checkbox("HUD lock (screen-anchored UI)", &wp.hudMask);
            if (wp.hudMask) {
                ImGui::SliderFloat("crosshair lock radius", &wp.hudCenterR, 0.0f, 0.20f, "%.3f");
                ImGui::SliderFloat("edge HUD inset", &wp.hudEdge, 0.0f, 0.20f, "%.3f");
                ImGui::SameLine(); ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Center disc locks the crosshair; edge inset locks the minimap/ammo/\n"
                                      "health corners. Locked = doesn't warp (a fixed reference). World\n"
                                      "under these zones won't reproject, so keep them just big enough.");
            }

            ImGui::Spacing();
            ImGui::Checkbox("HUD separation (hud-less compositor)", &wp.hudCompose);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ON: warp the hud-less scene and re-composite the UI unwarped (needs a\n"
                                  "clean hud-less buffer; prone to mask artifacts).\n"
                                  "OFF: warp the FINAL frame as one layer — the HUD swims slightly with\n"
                                  "the world, but there is NO mask, NO ghost, NO soup. Use this to judge\n"
                                  "the raw reprojection quality and as a clean low-latency baseline.");
            ImGui::BeginDisabled(!wp.hudCompose);
            ImGui::SliderFloat("UI threshold", &wp.uiThreshold, 0.0f, 0.20f, "%.4f");
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HUD-less compositor: |present - hudless| (max channel) above this is\n"
                                  "treated as UI and re-applied UNWARPED. Raise if film grain/post leaks\n"
                                  "the scene into the UI mask; lower if faint/translucent UI is lost.\n"
                                  "Only used when a hud-less buffer is captured.");
            ImGui::SliderInt("UI erode (px)", &wp.uiErode, 0, 2);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Erodes the UI mask: a lone high-delta film-grain pixel surrounded by\n"
                                  "background is rejected, so it can't ghost the unwarped frame into the\n"
                                  "warped scene (the blur that grows with gain). Higher = more grain\n"
                                  "rejection but thins fine UI. 1 is usually enough.");
            ImGui::Checkbox("debug: show UI mask", &wp.debugMask);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Output the UI mask as grayscale: white = treated as UI (kept unwarped),\n"
                                  "black = scene (warped). If the background isn't solid black while moving,\n"
                                  "the mask is leaking -> raise UI threshold and/or UI erode.");
            ImGui::EndDisabled();
        }

        if (Presenter::AsyncEnabled()) {
            ImGui::Separator();
            Presenter::PresenterParams& pp = Presenter::Params();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "ASYNC presenter ACTIVE");
            ImGui::Text("present %.0f fps   game %.0f fps   refresh %.1f Hz",
                        pp.presentFps, pp.gameFps, pp.refreshHz);
            ImGui::Text("presented %llu   game frames %llu",
                        (unsigned long long)pp.presented, (unsigned long long)pp.gameFrames);
            bool vsync = pp.syncInterval != 0;
            if (ImGui::Checkbox("vsync (sync interval 1)", &vsync)) pp.syncInterval = vsync ? 1 : 0;
            ImGui::Checkbox("late-warp (Reflex-2 pacing)", &pp.lateWarp);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sleep until just before vblank, then latch mouse + warp + present.\n"
                                  "Caps presents to the refresh and minimises input->photon latency.");
            ImGui::Checkbox("auto-lead", &pp.autoLead);
            ImGui::SameLine();
            if (pp.autoLead) ImGui::Text("vblank lead %.2f ms (auto)", pp.leadMs);
            else             ImGui::SliderFloat("vblank lead ms", &pp.leadMs, 0.5f, 8.0f, "%.2f");

            ImGui::SliderInt("max frames in flight", &pp.maxFramesInFlight, 0, 4);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Caps how far the game's CPU runs ahead of GPU completion. Our Present\n"
                                  "returns instantly, so a GPU-bound game queues deep and the freshest\n"
                                  "finished frame goes stale (rubberbanding). Lower = fresher frames +\n"
                                  "lower game-age; too low can cost throughput. 0 = off.");

            ImGui::Checkbox("adaptive delay (Anti-Lag)", &pp.adaptiveDelay);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POC: injects a self-tuning CPU delay at the game's Present so its next\n"
                                  "frame samples input later -> fresher content -> lower latency, without\n"
                                  "dropping fps. Tunes via a jitter+EWMA probe (Cyberpunk's sim is decoupled,\n"
                                  "so a fixed cap can't do this). A/B with this off.");
            if (pp.adaptiveDelay)
                ImGui::Text("  drain %.2f ms   sim gradient %.2f (1=floor, 0=backlogged)",
                            pp.drainMs, pp.simGradient);

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "latency");
            ImGui::Text("  input->scanout %5.2f ms", pp.inputAgeMs);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mouse latch -> frame at the vblank. This is what late-warp minimises;\n"
                                  "lower the vblank lead and watch this drop (until missed vblanks climb).");
            ImGui::Text("  game frame age %5.2f ms", pp.gameAgeMs);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Age of the re-presented game frame at latch time. Grows when present fps\n"
                                  ">> game fps (the gap the inserted/warped frames are covering).");
            ImGui::Text("  present jitter %5.2f ms   missed vblanks %llu",
                        pp.jitterMs, (unsigned long long)pp.missedVblanks);
            ImGui::Text("  GPU pipeline depth %.1f frames", pp.gpuDepth);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many frames the CPU leads GPU completion by. This is the floor on\n"
                                  "game-age: the freshest finished frame is this many frames old. Lower it\n"
                                  "with 'max frames in flight'. If this stays high with the limiter on, the\n"
                                  "game is ignoring the backpressure.");
        } else {
            ImGui::Separator();
            ImGui::TextDisabled("async presenter off (set ASYNCREPROJ_ASYNC=1)");
        }

        ImGui::Separator();
        ImGui::Checkbox("Capture debug views", &s_showDebug);
        if (s_showDebug)
            DrawDebugViews();
    }
    ImGui::End();
}

// Lazily build all ImGui + D3D12 state from the live swapchain. Returns true once ready.
bool EnsureInit(IDXGISwapChain* swapchain) {
    if (s_init)       return true;
    if (s_initFailed) return false;
    if (!s_queue || !swapchain) return false; // need the present queue first

    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc1))) || !sc1) {
        LOG_ERROR("Overlay: swapchain has no IDXGISwapChain1");
        s_initFailed = true;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    sc1->GetDesc1(&desc);
    s_bufferCount = desc.BufferCount ? desc.BufferCount : 2;
    s_rtvFormat   = desc.Format;

    if (FAILED(sc1->GetDevice(IID_PPV_ARGS(&s_device))) || !s_device) {
        LOG_ERROR("Overlay: failed to get device from swapchain");
        sc1->Release();
        s_initFailed = true;
        return false;
    }

    if (FAILED(sc1->GetHwnd(&s_hwnd)) || !s_hwnd)
        s_hwnd = HookManager::GetGameHwnd();
    sc1->Release();

    if (!s_hwnd) {
        LOG_ERROR("Overlay: no window handle available");
        s_initFailed = true;
        return false;
    }

    // SRV heap (shader-visible) — ImGui 1.92 allocates per dynamic texture; 64 is plenty for a menu.
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 64;
    srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&s_srvHeap)))) {
        LOG_ERROR("Overlay: CreateDescriptorHeap(SRV) failed");
        s_initFailed = true;
        return false;
    }
    s_srvAlloc.Create(s_device, s_srvHeap);

    // RTV heap — one per backbuffer.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = s_bufferCount;
    if (FAILED(s_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&s_rtvHeap)))) {
        LOG_ERROR("Overlay: CreateDescriptorHeap(RTV) failed");
        s_initFailed = true;
        return false;
    }
    s_rtvIncrement = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kFrames; ++i) {
        if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc[i])))) {
            LOG_ERROR("Overlay: CreateCommandAllocator failed");
            s_initFailed = true;
            return false;
        }
    }
    if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc[0], nullptr, IID_PPV_ARGS(&s_cmdList)))) {
        LOG_ERROR("Overlay: CreateCommandList failed");
        s_initFailed = true;
        return false;
    }
    s_cmdList->Close();

    if (FAILED(s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence)))) {
        LOG_ERROR("Overlay: CreateFence failed");
        s_initFailed = true;
        return false;
    }
    s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // don't litter the game folder with imgui.ini
    // Use a single real OS cursor (forced in HookedWndProc). Disable ImGui's software cursor and
    // tell the backend never to touch the OS cursor itself, so it can't fight the game over it.
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(s_hwnd)) {
        LOG_ERROR("Overlay: ImGui_ImplWin32_Init failed");
        s_initFailed = true;
        return false;
    }

    ImGui_ImplDX12_InitInfo info = {};
    info.Device               = s_device;
    info.CommandQueue         = s_queue;
    info.NumFramesInFlight    = kFrames;
    info.RTVFormat            = s_rtvFormat;
    info.SrvDescriptorHeap    = s_srvHeap;
    info.SrvDescriptorAllocFn = SrvAllocFn;
    info.SrvDescriptorFreeFn  = SrvFreeFn;
    if (!ImGui_ImplDX12_Init(&info)) {
        LOG_ERROR("Overlay: ImGui_ImplDX12_Init failed");
        ImGui_ImplWin32_Shutdown();
        s_initFailed = true;
        return false;
    }

    // Take over the window proc for input, and neutralize the game's cursor clip / recentre.
    s_oWndProc = (WNDPROC)SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    InstallInputHooks();

    s_init = true;
    LOG_INFO("Overlay: initialized (%ux%u, fmt=%u, buffers=%u, hwnd=0x%p)",
             desc.Width, desc.Height, (unsigned)s_rtvFormat, s_bufferCount, s_hwnd);
    return true;
}

} // namespace

namespace Overlay {

void SetPresentQueue(ID3D12CommandQueue* queue) {
    if (!queue || s_queue == queue) return;
    if (s_queue) s_queue->Release();
    s_queue = queue;
    s_queue->AddRef();
}

ID3D12CommandQueue* GetPresentQueue() { return s_queue; }

void RenderOverlay(IDXGISwapChain* swapchain) {
    if (!EnsureInit(swapchain))
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();

    // Resolve the current backbuffer.
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3)
        return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* backbuffer = nullptr;
    HRESULT hr = sc3->GetBuffer(idx, IID_PPV_ARGS(&backbuffer));
    sc3->Release();
    if (FAILED(hr) || !backbuffer)
        return;

    // Throttle the recording ring against the GPU.
    UINT slot = s_frameIdx % kFrames;
    if (s_fence->GetCompletedValue() < s_frameFence[slot]) {
        s_fence->SetEventOnCompletion(s_frameFence[slot], s_fenceEvent);
        WaitForSingleObject(s_fenceEvent, INFINITE);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * s_rtvIncrement;
    s_device->CreateRenderTargetView(backbuffer, nullptr, rtv);

    s_alloc[slot]->Reset();
    s_cmdList->Reset(s_alloc[slot], nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_cmdList->ResourceBarrier(1, &barrier);

    s_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { s_srvHeap };
    s_cmdList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), s_cmdList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s_cmdList->ResourceBarrier(1, &barrier);

    s_cmdList->Close();
    ID3D12CommandList* lists[] = { s_cmdList };
    s_queue->ExecuteCommandLists(1, lists);

    s_frameFence[slot] = ++s_fenceVal;
    s_queue->Signal(s_fence, s_fenceVal);
    s_frameIdx++;

    backbuffer->Release();
}

void Shutdown() {
    if (s_init) {
        // Drain the GPU so we don't free objects still in flight.
        if (s_queue && s_fence) {
            s_queue->Signal(s_fence, ++s_fenceVal);
            if (s_fence->GetCompletedValue() < s_fenceVal) {
                s_fence->SetEventOnCompletion(s_fenceVal, s_fenceEvent);
                WaitForSingleObject(s_fenceEvent, INFINITE);
            }
        }
        RemoveInputHooks();
        if (s_hwnd && s_oWndProc)
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_oWndProc);
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    for (UINT i = 0; i < kFrames; ++i) { if (s_alloc[i]) { s_alloc[i]->Release(); s_alloc[i] = nullptr; } }
    if (s_cmdList) { s_cmdList->Release(); s_cmdList = nullptr; }
    if (s_fence)   { s_fence->Release();   s_fence = nullptr; }
    if (s_fenceEvent) { CloseHandle(s_fenceEvent); s_fenceEvent = nullptr; }
    if (s_rtvHeap) { s_rtvHeap->Release(); s_rtvHeap = nullptr; }
    if (s_srvHeap) { s_srvHeap->Release(); s_srvHeap = nullptr; }
    if (s_device)  { s_device->Release();  s_device = nullptr; }
    if (s_queue)   { s_queue->Release();   s_queue = nullptr; }

    s_init = false;
    s_oWndProc = nullptr;
    s_hwnd = nullptr;
}

} // namespace Overlay
