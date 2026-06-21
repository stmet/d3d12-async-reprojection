#include "depth_capture.h"
#include "ffx_defs.h"
#include "../common/logger.h"
#include <detours.h>
#include <mutex>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler")

namespace DepthCapture {
namespace {

std::mutex        s_mtx;
ID3D12Resource*   s_depth = nullptr;
ID3D12Resource*   s_mv    = nullptr;
Cam               s_cam;
bool              s_have  = false;

// ---- depth copy (our own stable, simultaneous-access texture the warp samples) ----
ID3D12Device*     s_device   = nullptr;
ID3D12Resource*   s_depthCopy = nullptr;
UINT              s_copyW = 0, s_copyH = 0;
DXGI_FORMAT       s_copyFmt = DXGI_FORMAT_UNKNOWN;   // texture format (typeless family)
DXGI_FORMAT       s_srvFmt  = DXGI_FORMAT_UNKNOWN;   // SRV view format
ID3D12Resource*   s_lastCopiedDepth = nullptr;       // dedup: both hooks may pass the same depth/frame

// FFX native FfxResourceStates (bit flags) -> D3D12 states (for transitioning the game's depth).
D3D12_RESOURCE_STATES FfxStateToD3D12(uint32_t s) {
    D3D12_RESOURCE_STATES d = (D3D12_RESOURCE_STATES)0;
    if (s & 0x02) d |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & 0x04) d |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & 0x08) d |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & 0x10) d |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & 0x20) d |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & 0x100) d |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s & 0x200) d |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return d ? d : D3D12_RESOURCE_STATE_COMMON;
}

void PickDepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* srv) {
    switch (f) {
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
            *tex = DXGI_FORMAT_R32_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT; break;
        // 64-bit depth+stencil (Cyberpunk: D32_FLOAT_S8X24 / R32G8X24_TYPELESS = 19). SRV reads the
        // R32 depth plane via R32_FLOAT_X8X24_TYPELESS — a typeless SRV format is INVALID and removes
        // the device.
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *tex = DXGI_FORMAT_R32G8X24_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
            *tex = DXGI_FORMAT_R24G8_TYPELESS; *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
            *tex = DXGI_FORMAT_R16_TYPELESS; *srv = DXGI_FORMAT_R16_UNORM; break;
        default: *tex = f; *srv = f; break;   // already a concrete, SRV-able color format
    }
}

inline void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

// Record a copy of the game's depth into our simultaneous-access texture onto the game's OWN command
// list, while the depth is still in its FSR-input state (ffxState). Lazily (re)creates the texture.
void RecordDepthCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* depth, uint32_t ffxState) {
    if (!cl || !depth) return;
    if (!s_device && FAILED(depth->GetDevice(IID_PPV_ARGS(&s_device)))) return;
    D3D12_RESOURCE_DESC dd = depth->GetDesc();
    DXGI_FORMAT texFmt, srvFmt; PickDepthFormats(dd.Format, &texFmt, &srvFmt);
    if (!s_depthCopy || s_copyW != (UINT)dd.Width || s_copyH != dd.Height || s_copyFmt != texFmt) {
        if (s_depthCopy) { s_depthCopy->Release(); s_depthCopy = nullptr; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = dd.Width; rd.Height = dd.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = texFmt; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(s_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_depthCopy)))) {
            LOG_ERROR("DepthCapture: failed to create depth-copy tex %llux%u fmt=%u", (unsigned long long)dd.Width, dd.Height, (unsigned)texFmt);
            return;
        }
        s_copyW = (UINT)dd.Width; s_copyH = dd.Height; s_copyFmt = texFmt;
        { std::lock_guard<std::mutex> lk(s_mtx); s_srvFmt = srvFmt; }
        LOG_INFO("DepthCapture: depth-copy texture %ux%u texFmt=%u srvFmt=%u", s_copyW, s_copyH, (unsigned)texFmt, (unsigned)srvFmt);
    }
    D3D12_RESOURCE_STATES src = FfxStateToD3D12(ffxState);
    Barrier(cl, depth, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, s_depthCopy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(s_depthCopy, depth);
    Barrier(cl, s_depthCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, src);
}

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
// Take the desc as void* and cast to the layout matching each entry point (they differ: the combined
// FSR3-context dispatch has 7 resources, the decoupled upscaler has 10).
typedef int (WINAPI* PFN_Fsr3Dispatch)(void*, const void*);
PFN_Fsr3Dispatch o_fsr3CtxDispatchUpscale  = nullptr;
PFN_Fsr3Dispatch o_fsr3UpscalerCtxDispatch = nullptr;

// Shared: store latest depth/mv/cam and copy depth on the game's command list.
void HandleDispatch(void* commandList, void* depthRes, uint32_t depthState,
                    void* mvRes, uint32_t mvState, const Cam& cam) {
    OnUpscaleDispatch((ID3D12Resource*)depthRes, (ID3D12Resource*)mvRes, cam, depthState, mvState);
    if (commandList && depthRes)
        RecordDepthCopy((ID3D12GraphicsCommandList*)commandList, (ID3D12Resource*)depthRes, depthState);
}

int WINAPI hkFsr3CtxDispatchUpscale(void* ctx, const void* desc) {
    static bool logged = false;
    if (!logged) { logged = true; LOG_INFO("DepthCapture: upscale dispatched via ffxFsr3ContextDispatchUpscale (combined layout)"); }
    const FfxFsr3DispatchUpscaleDescCombined* d = (const FfxFsr3DispatchUpscaleDescCombined*)desc;
    if (d) {
        Cam cam; cam.nearZ = d->cameraNear; cam.farZ = d->cameraFar; cam.fovV = d->cameraFovAngleVertical;
        cam.renderW = d->renderSize.width; cam.renderH = d->renderSize.height;
        HandleDispatch(d->commandList, d->depth.resource, (uint32_t)d->depth.state,
                       d->motionVectors.resource, (uint32_t)d->motionVectors.state, cam);
    }
    return o_fsr3CtxDispatchUpscale(ctx, desc);
}
int WINAPI hkFsr3UpscalerCtxDispatch(void* ctx, const void* desc) {
    static bool logged = false;
    if (!logged) { logged = true; LOG_INFO("DepthCapture: upscale dispatched via ffxFsr3UpscalerContextDispatch (upscaler layout)"); }
    const FfxFsr3DispatchDescription* d = (const FfxFsr3DispatchDescription*)desc;
    if (d) {
        Cam cam; cam.nearZ = d->cameraNear; cam.farZ = d->cameraFar; cam.fovV = d->cameraFovAngleVertical;
        cam.renderW = d->renderSize.width; cam.renderH = d->renderSize.height;
        HandleDispatch(d->commandList, d->depth.resource, (uint32_t)d->depth.state,
                       d->motionVectors.resource, (uint32_t)d->motionVectors.state, cam);
    }
    return o_fsr3UpscalerCtxDispatch(ctx, desc);
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

// ---- ADS-by-depth-profile: count near-field samples in a center-lower region of the depth ----
constexpr UINT kCovRing = 4;
constexpr UINT kCovGridX = 24, kCovGridY = 20;     // 480 samples over the ROI
ID3D12RootSignature*       s_covRoot = nullptr;
ID3D12PipelineState*       s_covPso  = nullptr;
ID3D12DescriptorHeap*      s_covHeap = nullptr;     // [0]=SRV depth, [1]=UAV count
ID3D12Resource*            s_covCount = nullptr;    // DEFAULT, 1 uint (UAV)
ID3D12Resource*            s_covReadback = nullptr; // READBACK, kCovRing uints
UINT*                      s_covMapped = nullptr;
ID3D12CommandAllocator*    s_covAlloc[kCovRing] = {};
ID3D12GraphicsCommandList* s_covList = nullptr;
ID3D12Fence*               s_covFence = nullptr;
UINT64                     s_covVal = 0;
UINT64                     s_covSlotFence[kCovRing] = {};
UINT                       s_covIdx = 0;
UINT                       s_covInc = 0;
bool                       s_covInit = false, s_covFailed = false;
float                      s_coverage = 0.0f;

const char* kCoverageCS =
"Texture2D<float>      gDepth : register(t0);\n"
"RWByteAddressBuffer   gOut   : register(u0);\n"
"SamplerState          gPt   : register(s0);\n"
"cbuffer P : register(b0) { float gNearCut; float gX0; float gX1; float gY0; float gY1; uint gGX; uint gGY; uint gPad; };\n"
"[numthreads(1,1,1)]\n"
"void CSMain() {\n"
"    uint n = 0;\n"
"    [loop] for (uint y = 0; y < gGY; ++y)\n"
"      [loop] for (uint x = 0; x < gGX; ++x) {\n"
"          float2 uv = float2(gX0 + (x + 0.5f) / gGX * (gX1 - gX0), gY0 + (y + 0.5f) / gGY * (gY1 - gY0));\n"
"          if (gDepth.SampleLevel(gPt, uv, 0) > gNearCut) ++n;\n"
"      }\n"
"    gOut.Store(0, n);\n"
"}\n";

bool BuildCoveragePipeline() {
    if (s_covInit) return true;
    if (s_covFailed || !s_device) return false;

    // Separate descriptor tables for SRV and UAV (most robust), each starting at its own bound handle.
    D3D12_DESCRIPTOR_RANGE rSrv = {}; rSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rSrv.NumDescriptors = 1; rSrv.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE rUav = {}; rUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rUav.NumDescriptors = 1; rUav.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER p[3] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[0].DescriptorTable.NumDescriptorRanges = 1; p[0].DescriptorTable.pDescriptorRanges = &rSrv;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &rUav;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[2].Constants.ShaderRegister = 0; p[2].Constants.Num32BitValues = 8;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 3; rs.pParameters = p; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { if (sig) sig->Release(); if (err) err->Release(); s_covFailed = true; return false; }
    HRESULT hr = s_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s_covRoot));
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { s_covFailed = true; return false; }
    ID3DBlob* cs = nullptr; ID3DBlob* e1 = nullptr;
    if (FAILED(D3DCompile(kCoverageCS, strlen(kCoverageCS), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &cs, &e1))) {
        LOG_ERROR("DepthCapture: coverage CS compile failed: %s", e1 ? (char*)e1->GetBufferPointer() : "?");
        if (cs) cs->Release(); if (e1) e1->Release(); s_covFailed = true; return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {}; cp.pRootSignature = s_covRoot; cp.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = s_device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&s_covPso)); cs->Release(); if (e1) e1->Release();
    if (FAILED(hr)) { s_covFailed = true; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_covHeap)))) { s_covFailed = true; return false; }
    s_covInc = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES dh = {}; dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = sizeof(UINT); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(s_device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_covCount)))) { s_covFailed = true; return false; }
    D3D12_HEAP_PROPERTIES rh = {}; rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = sizeof(UINT) * kCovRing; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_covReadback)))) { s_covFailed = true; return false; }
    D3D12_RANGE none = { 0, 0 }; s_covReadback->Map(0, &none, (void**)&s_covMapped);

    // DIRECT type: this runs on the present (direct) queue, which cannot execute a COMPUTE-type list.
    // A direct command list can still record compute dispatches.
    for (UINT i = 0; i < kCovRing; ++i)
        if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_covAlloc[i])))) { s_covFailed = true; return false; }
    if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_covAlloc[0], nullptr, IID_PPV_ARGS(&s_covList)))) { s_covFailed = true; return false; }
    s_covList->Close();
    s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_covFence));
    s_covInit = true;
    LOG_INFO("DepthCapture: ADS coverage pipeline built");
    return true;
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

bool GetDepthSRV(ID3D12Resource** tex, DXGI_FORMAT* srvFmt) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_depthCopy) return false;
    if (tex)    *tex    = s_depthCopy;
    if (srvFmt) *srvFmt = s_srvFmt;
    return true;
}

float GetNearCoverage() { return s_coverage; }

void ComputeNearCoverage(ID3D12CommandQueue* queue, float nearCut) {
    ID3D12Resource* depthTex = nullptr; DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN;
    { std::lock_guard<std::mutex> lk(s_mtx); depthTex = s_depthCopy; srvFmt = s_srvFmt; }
    if (!queue || !depthTex) return;
    if (!BuildCoveragePipeline()) return;

    UINT slot = s_covIdx % kCovRing;
    // Read this slot's result from kCovRing dispatches ago (complete by now).
    if (s_covSlotFence[slot] != 0 && s_covFence->GetCompletedValue() >= s_covSlotFence[slot])
        s_coverage = (float)s_covMapped[slot] / (float)(kCovGridX * kCovGridY);

    // Descriptors: [0]=SRV depth, [1]=UAV count.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_covHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_covHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = srvFmt; sv.Texture2D.MipLevels = 1;
    s_device->CreateShaderResourceView(depthTex, &sv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = cpu; uavCpu.ptr += s_covInc;
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpu; uavGpu.ptr += s_covInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.FirstElement = 0; uav.Buffer.NumElements = 1; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    s_device->CreateUnorderedAccessView(s_covCount, nullptr, &uav, uavCpu);

    s_covAlloc[slot]->Reset();
    s_covList->Reset(s_covAlloc[slot], s_covPso);
    Barrier(s_covList, s_covCount, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s_covList->SetComputeRootSignature(s_covRoot);
    ID3D12DescriptorHeap* heaps[] = { s_covHeap };
    s_covList->SetDescriptorHeaps(1, heaps);
    s_covList->SetComputeRootDescriptorTable(0, gpu);      // SRV (depth)
    s_covList->SetComputeRootDescriptorTable(1, uavGpu);   // UAV (count)
    struct { float nearCut, x0, x1, y0, y1; UINT gx, gy, pad; } c =
        { nearCut, 0.35f, 0.65f, 0.40f, 0.92f, kCovGridX, kCovGridY, 0 };
    s_covList->SetComputeRoot32BitConstants(2, 8, &c, 0);
    s_covList->Dispatch(1, 1, 1);
    Barrier(s_covList, s_covCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s_covList->CopyBufferRegion(s_covReadback, (UINT64)slot * sizeof(UINT), s_covCount, 0, sizeof(UINT));
    Barrier(s_covList, s_covCount, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    s_covList->Close();
    ID3D12CommandList* lists[] = { s_covList };
    queue->ExecuteCommandLists(1, lists);
    s_covSlotFence[slot] = ++s_covVal;
    queue->Signal(s_covFence, s_covVal);
    s_covIdx++;
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
    { std::lock_guard<std::mutex> lk(s_mtx);
      if (s_depthCopy) { s_depthCopy->Release(); s_depthCopy = nullptr; }
      if (s_device)    { s_device->Release();    s_device = nullptr; } }
}

} // namespace DepthCapture
