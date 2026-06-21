#include "depth_capture.h"
#include "ffx_defs.h"
#include "../common/logger.h"
#include <detours.h>
#include <mutex>

namespace DepthCapture {
namespace {

std::mutex        s_mtx;
ID3D12Resource*   s_depth = nullptr;
ID3D12Resource*   s_mv    = nullptr;
Cam               s_cam;
bool              s_have  = false;

// ---- ffxDispatch detour ----
typedef ffxReturnCode_t (WINAPI* PFN_ffxDispatch)(ffxContext*, const ffxDispatchDescHeader*);
PFN_ffxDispatch   o_ffxDispatch = nullptr;
bool              s_ffxHooked   = false;

ffxReturnCode_t WINAPI hkffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    // Walk the desc chain; the upscale dispatch carries the depth/MV/camera we want.
    for (const ffxApiHeader* h = desc; h; h = h->pNext) {
        if (h->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
            const ffxDispatchDescUpscale* up = (const ffxDispatchDescUpscale*)h;
            Cam cam;
            cam.nearZ = up->cameraNear; cam.farZ = up->cameraFar; cam.fovV = up->cameraFovAngleVertical;
            cam.renderW = up->renderSize.width; cam.renderH = up->renderSize.height;
            OnUpscaleDispatch((ID3D12Resource*)up->depth.resource,
                              (ID3D12Resource*)up->motionVectors.resource,
                              cam, up->depth.state, up->motionVectors.state);
            break;
        }
    }
    return o_ffxDispatch(context, desc);
}

void HookFfxModule(HMODULE mod) {
    if (s_ffxHooked || !mod) return;
    auto p = (PFN_ffxDispatch)GetProcAddress(mod, "ffxDispatch");
    if (!p) return;   // not the ffx-api DLL (or a build without the export)
    o_ffxDispatch = p;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o_ffxDispatch, hkffxDispatch);
    if (DetourTransactionCommit() == NO_ERROR) {
        s_ffxHooked = true;
        LOG_INFO("DepthCapture: hooked ffxDispatch (FSR3.1 ffx-api) in 0x%p", mod);
    } else {
        o_ffxDispatch = nullptr;
        LOG_ERROR("DepthCapture: failed to detour ffxDispatch");
    }
}

// ---- native FSR3 SDK dispatch detours (FSR 3.0 — Cyberpunk's path) ----
typedef int (WINAPI* PFN_Fsr3Dispatch)(void*, const FfxFsr3DispatchDescription*);
PFN_Fsr3Dispatch o_fsr3CtxDispatchUpscale  = nullptr;
PFN_Fsr3Dispatch o_fsr3UpscalerCtxDispatch = nullptr;

void HandleFsr3(const FfxFsr3DispatchDescription* d) {
    if (!d) return;
    Cam cam;
    cam.nearZ = d->cameraNear; cam.farZ = d->cameraFar; cam.fovV = d->cameraFovAngleVertical;
    cam.renderW = d->renderSize.width; cam.renderH = d->renderSize.height;
    OnUpscaleDispatch((ID3D12Resource*)d->depth.resource,
                      (ID3D12Resource*)d->motionVectors.resource,
                      cam, (uint32_t)d->depth.state, (uint32_t)d->motionVectors.state);
}

int WINAPI hkFsr3CtxDispatchUpscale(void* ctx, const FfxFsr3DispatchDescription* d) {
    static bool logged = false;
    if (!logged) { logged = true; LOG_INFO("DepthCapture: upscale dispatched via ffxFsr3ContextDispatchUpscale"); }
    HandleFsr3(d); return o_fsr3CtxDispatchUpscale(ctx, d);
}
int WINAPI hkFsr3UpscalerCtxDispatch(void* ctx, const FfxFsr3DispatchDescription* d) {
    static bool logged = false;
    if (!logged) { logged = true; LOG_INFO("DepthCapture: upscale dispatched via ffxFsr3UpscalerContextDispatch"); }
    HandleFsr3(d); return o_fsr3UpscalerCtxDispatch(ctx, d);
}

void HookFsr3Module(HMODULE mod) {
    if (!mod) return;
    if (!o_fsr3CtxDispatchUpscale) {
        if (auto p = (PFN_Fsr3Dispatch)GetProcAddress(mod, "ffxFsr3ContextDispatchUpscale")) {
            o_fsr3CtxDispatchUpscale = p;
            DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_fsr3CtxDispatchUpscale, hkFsr3CtxDispatchUpscale);
            if (DetourTransactionCommit() == NO_ERROR)
                LOG_INFO("DepthCapture: hooked ffxFsr3ContextDispatchUpscale (FSR3.0) in 0x%p", mod);
            else o_fsr3CtxDispatchUpscale = nullptr;
        }
    }
    if (!o_fsr3UpscalerCtxDispatch) {
        if (auto p = (PFN_Fsr3Dispatch)GetProcAddress(mod, "ffxFsr3UpscalerContextDispatch")) {
            o_fsr3UpscalerCtxDispatch = p;
            DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_fsr3UpscalerCtxDispatch, hkFsr3UpscalerCtxDispatch);
            if (DetourTransactionCommit() == NO_ERROR)
                LOG_INFO("DepthCapture: hooked ffxFsr3UpscalerContextDispatch (FSR3.0) in 0x%p", mod);
            else o_fsr3UpscalerCtxDispatch = nullptr;
        }
    }
}

// Late-hook the upscaler DLL the instant the game loads it (they load after us). Match by substring,
// case-insensitive: "amd_fidelityfx_dx12" (FSR3.1 ffx-api) or "ffx_fsr3" (FSR3.0 native upscaler).
void MaybeHook(const wchar_t* name, HMODULE m) {
    if (!name || !m) return;
    wchar_t lower[MAX_PATH]; size_t n = 0;
    for (; name[n] && n < MAX_PATH - 1; ++n) lower[n] = (wchar_t)towlower(name[n]);
    lower[n] = 0;
    if (wcsstr(lower, L"amd_fidelityfx_dx12")) HookFfxModule(m);
    if (wcsstr(lower, L"ffx_fsr3"))            HookFsr3Module(m);
}

typedef HMODULE (WINAPI* PFN_LLW)(LPCWSTR);
typedef HMODULE (WINAPI* PFN_LLExW)(LPCWSTR, HANDLE, DWORD);
PFN_LLW   o_LoadLibraryW   = nullptr;
PFN_LLExW o_LoadLibraryExW = nullptr;

HMODULE WINAPI hkLoadLibraryW(LPCWSTR name) {
    HMODULE m = o_LoadLibraryW(name);
    MaybeHook(name, m);
    return m;
}
HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags) {
    HMODULE m = o_LoadLibraryExW(name, f, flags);
    MaybeHook(name, m);
    return m;
}

} // namespace

void OnUpscaleDispatch(ID3D12Resource* depth, ID3D12Resource* mv, const Cam& cam,
                       uint32_t depthFfxState, uint32_t mvFfxState) {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_depth = depth; s_mv = mv; s_cam = cam; s_have = (depth != nullptr);
    }
    // Log only when the resource identity changes, to avoid flooding (fires once per scene/resize).
    static ID3D12Resource* lastD = (ID3D12Resource*)-1; static ID3D12Resource* lastM = (ID3D12Resource*)-1;
    if (depth != lastD || mv != lastM) {
        lastD = depth; lastM = mv;
        D3D12_RESOURCE_DESC dd = {}; if (depth) dd = depth->GetDesc();
        LOG_INFO("DepthCapture: depth=0x%p (%llux%u fmt=%u state=%u) mv=0x%p (state=%u) | render=%ux%u near=%.4f far=%.1f fovV=%.3f",
                 depth, (unsigned long long)dd.Width, dd.Height, (unsigned)dd.Format, depthFfxState,
                 mv, mvFfxState, cam.renderW, cam.renderH, cam.nearZ, cam.farZ, cam.fovV);
    }
}

bool GetLatest(ID3D12Resource** depth, ID3D12Resource** mv, Cam* cam) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_have) return false;
    if (depth) *depth = s_depth;
    if (mv)    *mv    = s_mv;
    if (cam)   *cam   = s_cam;
    return true;
}

void Install() {
    HMODULE u32 = GetModuleHandleW(L"kernel32.dll");
    if (u32) {
        o_LoadLibraryW   = (PFN_LLW)  GetProcAddress(u32, "LoadLibraryW");
        o_LoadLibraryExW = (PFN_LLExW)GetProcAddress(u32, "LoadLibraryExW");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (o_LoadLibraryW)   DetourAttach(&(PVOID&)o_LoadLibraryW,   hkLoadLibraryW);
        if (o_LoadLibraryExW) DetourAttach(&(PVOID&)o_LoadLibraryExW, hkLoadLibraryExW);
        if (DetourTransactionCommit() != NO_ERROR)
            LOG_ERROR("DepthCapture: failed to detour LoadLibrary");
    }
    // In case the upscaler DLLs are already loaded by the time we install.
    if (HMODULE m = GetModuleHandleW(L"amd_fidelityfx_dx12.dll")) HookFfxModule(m);
    const wchar_t* fsr3Dlls[] = { L"ffx_fsr3upscaler_x64.dll", L"ffx_fsr3_x64.dll",
                                  L"ffx_fsr3_dx12_x64.dll", L"ffx_fsr3_dx12.dll" };
    for (auto d : fsr3Dlls) if (HMODULE m = GetModuleHandleW(d)) HookFsr3Module(m);
    LOG_INFO("DepthCapture: installed (FSR3.0 native + FSR3.1 ffx-api depth/MV interception)");
}

void Uninstall() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_LoadLibraryW)   DetourDetach(&(PVOID&)o_LoadLibraryW,   hkLoadLibraryW);
    if (o_LoadLibraryExW) DetourDetach(&(PVOID&)o_LoadLibraryExW, hkLoadLibraryExW);
    if (s_ffxHooked && o_ffxDispatch) DetourDetach(&(PVOID&)o_ffxDispatch, hkffxDispatch);
    if (o_fsr3CtxDispatchUpscale)  DetourDetach(&(PVOID&)o_fsr3CtxDispatchUpscale, hkFsr3CtxDispatchUpscale);
    if (o_fsr3UpscalerCtxDispatch) DetourDetach(&(PVOID&)o_fsr3UpscalerCtxDispatch, hkFsr3UpscalerCtxDispatch);
    DetourTransactionCommit();
    s_ffxHooked = false;
}

} // namespace DepthCapture
