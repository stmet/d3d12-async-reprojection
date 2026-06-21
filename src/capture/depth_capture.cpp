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
        LOG_INFO("DepthCapture: hooked ffxDispatch in 0x%p", mod);
    } else {
        o_ffxDispatch = nullptr;
        LOG_ERROR("DepthCapture: failed to detour ffxDispatch");
    }
}

// Late-hook the ffx-api DLL the instant the game loads it (it loads after us).
bool IsFfxDll(const wchar_t* name) {
    if (!name) return false;
    // match "amd_fidelityfx_dx12" anywhere in the path, case-insensitive
    wchar_t lower[MAX_PATH]; size_t n = 0;
    for (; name[n] && n < MAX_PATH - 1; ++n) lower[n] = (wchar_t)towlower(name[n]);
    lower[n] = 0;
    return wcsstr(lower, L"amd_fidelityfx_dx12") != nullptr;
}

typedef HMODULE (WINAPI* PFN_LLW)(LPCWSTR);
typedef HMODULE (WINAPI* PFN_LLExW)(LPCWSTR, HANDLE, DWORD);
PFN_LLW   o_LoadLibraryW   = nullptr;
PFN_LLExW o_LoadLibraryExW = nullptr;

HMODULE WINAPI hkLoadLibraryW(LPCWSTR name) {
    HMODULE m = o_LoadLibraryW(name);
    if (m && IsFfxDll(name)) HookFfxModule(m);
    return m;
}
HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags) {
    HMODULE m = o_LoadLibraryExW(name, f, flags);
    if (m && IsFfxDll(name)) HookFfxModule(m);
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
    // In case the ffx-api DLL is already loaded by the time we install.
    if (HMODULE m = GetModuleHandleW(L"amd_fidelityfx_dx12.dll")) HookFfxModule(m);
    LOG_INFO("DepthCapture: installed (ffx-api depth/MV interception)");
}

void Uninstall() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_LoadLibraryW)   DetourDetach(&(PVOID&)o_LoadLibraryW,   hkLoadLibraryW);
    if (o_LoadLibraryExW) DetourDetach(&(PVOID&)o_LoadLibraryExW, hkLoadLibraryExW);
    if (s_ffxHooked && o_ffxDispatch) DetourDetach(&(PVOID&)o_ffxDispatch, hkffxDispatch);
    DetourTransactionCommit();
    s_ffxHooked = false;
}

} // namespace DepthCapture
