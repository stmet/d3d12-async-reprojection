#include "overlay.h"
#include "../common/logger.h"
#include "../hooks/hook_manager.h"
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
        ImGui::Text("Lean async reprojection");
        ImGui::Text("Application %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::Separator();
        WarpParams& wp = WarpRenderer::Params();
        ImGui::Checkbox("Enable warp", &wp.enable);
        // Lean build captures no depth/MV, so only the two geometry-free warps are live: a flat UV
        // shift (mode 0) and the FOV-correct perspective rotation (mode 4, the default).
        int modeIdx = (wp.mode == 4) ? 1 : 0;
        if (ImGui::Combo("mode", &modeIdx, "Rotational shift\0Perspective rotational\0"))
            wp.mode = (modeIdx == 1) ? 4 : 0;
        // ---- gain: angular model (FOV-correct deg/count, the lean default) vs legacy flat UV gain ----
        ImGui::Checkbox("angular gain (FOV-correct deg/count)", &wp.angularGain);
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Maps mouse counts to camera yaw/pitch in DEGREES, then lets the perspective\n"
                              "projection turn that into the on-screen shift. The screen motion then scales\n"
                              "AUTOMATICALLY with FOV (zoom/ADS) -- the sensitivity is ONE constant you set\n"
                              "once and never re-tune per situation. DPI is baked into the raw counts and\n"
                              "cancels, so it is NOT a separate input. This replaces the old measure-the-\n"
                              "slope auto-gain (a noisy loop on a visible param -> the breathing/rubberband).\n"
                              "Off = legacy flat UV gain (FOV-agnostic).");
        if (wp.angularGain) {
            ImGui::SliderFloat("sensitivity (deg/1000 counts)", &wp.sensDegPer1000, 0.5f, 12.0f, "%.2f");
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The one FOV-independent constant. Calibrate ONCE by eye: aim at a sharp\n"
                                  "vertical edge, flick left/right, and raise/lower this until the edge holds\n"
                                  "still under the crosshair during the flick. Then leave it -- it never needs\n"
                                  "to change with FOV/zoom/scene (that is handled automatically).");
            ImGui::SliderFloat("vert:horiz ratio", &wp.pitchRatio, 0.5f, 1.5f, "%.2f");
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

        ImGui::Checkbox("auto-suppress in menus", &wp.menuDetect);
        ImGui::SameLine();
        if (wp.menuDetect)
            ImGui::TextColored(wp.runtimeSuppress ? ImVec4(1.0f, 0.7f, 0.2f, 1.0f) : ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               wp.runtimeSuppress ? "[MENU: warp off]" : "[gameplay]");
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Detects menus/pause/inventory via the game's cursor-clip state and turns the\n"
                              "warp off there (the camera isn't moving, so warping just swims the UI). Turn\n"
                              "off if a game doesn't clip the cursor during normal gameplay.");

        if (wp.mode == 4) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "perspective rotational (fold-free, depth-independent)");
            ImGui::SliderFloat("vertical FOV (deg)", &wp.fovDeg, 30.0f, 110.0f, "%.0f");
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Manual vertical FOV for the perspective warp (the lean build has no FSR\n"
                                  "dispatch capture to read it from). Set it to match the game's CURRENT FOV.\n"
                                  "With angular gain on, this is what makes the warp magnitude correct: when\n"
                                  "you zoom/ADS, lower this to match and the on-screen motion scales right.");
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
            if (pp.autoLead) {
                ImGui::SliderFloat("lead floor ms", &pp.leadFloorMs, 0.1f, 3.0f, "%.2f");
                ImGui::SameLine(); ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hard floor the auto-lead controller won't creep below. This is the\n"
                                      "latency knob: lower it to chase input->photon down, raise it if\n"
                                      "'missed vblanks' starts climbing. The controller settles just above\n"
                                      "this once the warp's GPU cost fits.");
            }

            ImGui::SliderInt("max frames in flight", &pp.maxFramesInFlight, 0, 4);
            ImGui::SameLine(); ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Caps how far the game's CPU runs ahead of GPU completion. Our Present\n"
                                  "returns instantly, so a GPU-bound game queues deep and the freshest\n"
                                  "finished frame goes stale (rubberbanding). Lower = fresher frames +\n"
                                  "lower game-age; too low can cost throughput. 0 = off.");

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
            ImGui::TextDisabled("async presenter off (forced via ASYNCREPROJ_ASYNC=0)");
        }
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
    if (HRESULT hr = s_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&s_srvHeap)); FAILED(hr)) {
        LOG_ERROR("Overlay: CreateDescriptorHeap(SRV) failed hr=0x%08X deviceRemovedReason=0x%08X",
                  (unsigned)hr, (unsigned)s_device->GetDeviceRemovedReason());
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

bool InGameMenu() {
    // Our own tuning overlay forces the cursor visible — that's not a game menu; keep the warp running.
    if (s_visible) return false;
    // Gameplay hides the OS cursor (mouselook); menus/pause/inventory/dialogue show it.
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci)) return (ci.flags & CURSOR_SHOWING) != 0;
    return false;
}

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
