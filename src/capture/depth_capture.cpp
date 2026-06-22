#include "depth_capture.h"
#include "ffx_defs.h"
#include "../common/logger.h"
#include <detours.h>
#include <mutex>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
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

// ---- MV copy (stable simultaneous-access texture, mirror of the depth copy) ----
ID3D12Resource*   s_mvCopy  = nullptr;
UINT              s_mvW = 0, s_mvH = 0;
DXGI_FORMAT       s_mvTexFmt = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT       s_mvSrvFmt = DXGI_FORMAT_UNKNOWN;

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
        // R32 depth plane via R32_FLOAT_X8X24_TYPELESS â€” a typeless SRV format is INVALID and removes
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

// Same idea for the motion-vector field: copy it on the game's command list while it's in its FSR-input
// state, into our own simultaneous-access texture the present-queue fit can sample with no barrier. MV is
// a concrete color format (R16G16_FLOAT etc.), so the texture/SRV format is the resource format as-is.
void RecordMvCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* mv, uint32_t ffxState) {
    if (!cl || !mv) return;
    if (!s_device && FAILED(mv->GetDevice(IID_PPV_ARGS(&s_device)))) return;
    D3D12_RESOURCE_DESC md = mv->GetDesc();
    DXGI_FORMAT texFmt, srvFmt; PickDepthFormats(md.Format, &texFmt, &srvFmt);
    if (!s_mvCopy || s_mvW != (UINT)md.Width || s_mvH != md.Height || s_mvTexFmt != texFmt) {
        if (s_mvCopy) { s_mvCopy->Release(); s_mvCopy = nullptr; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = md.Width; rd.Height = md.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = texFmt; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(s_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_mvCopy)))) {
            LOG_ERROR("DepthCapture: failed to create mv-copy tex %llux%u fmt=%u", (unsigned long long)md.Width, md.Height, (unsigned)texFmt);
            return;
        }
        s_mvW = (UINT)md.Width; s_mvH = md.Height; s_mvTexFmt = texFmt;
        { std::lock_guard<std::mutex> lk(s_mtx); s_mvSrvFmt = srvFmt; }
        LOG_INFO("DepthCapture: mv-copy texture %ux%u texFmt=%u srvFmt=%u", s_mvW, s_mvH, (unsigned)texFmt, (unsigned)srvFmt);
    }
    D3D12_RESOURCE_STATES src = FfxStateToD3D12(ffxState);
    Barrier(cl, mv, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, s_mvCopy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(s_mvCopy, mv);
    Barrier(cl, s_mvCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, src);
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

// ---- native FSR3 SDK dispatch detours (FSR 3.0 â€” Cyberpunk's path) ----
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
    if (commandList && mvRes)
        RecordMvCopy((ID3D12GraphicsCommandList*)commandList, (ID3D12Resource*)mvRes, mvState);
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

// ---- Phase 3: global camera-translation least-squares fit from the MV field ----
// A single-thread compute pass walks a grid over the frame; for each world pixel it forms the parallax
// design rows (depth-weighted) and accumulates the 3x3 normal equations (ATA, ATb) for the camera
// translation, storing them once. The tiny system is solved on the CPU after a few-frame-latent readback.
constexpr UINT kFitGX = 48, kFitGY = 27;     // ~1300 samples across the frame
constexpr UINT kFitRing = 4;
constexpr UINT kFitU = 24;                    // uints per slot (21 used: 5x5 upper-tri M[15], b[5], cnt)
ID3D12RootSignature*       s_fitRoot = nullptr;
ID3D12PipelineState*       s_fitPso  = nullptr;
ID3D12DescriptorHeap*      s_fitHeap = nullptr;   // [0]=mv SRV (t0), [1]=depth SRV (t1), [2]=accum UAV (u0)
ID3D12Resource*            s_fitAccum = nullptr;  // DEFAULT, kFitU uints (raw UAV)
ID3D12Resource*            s_fitReadback = nullptr;
UINT*                      s_fitMapped = nullptr;
ID3D12CommandAllocator*    s_fitAlloc[kFitRing] = {};
ID3D12GraphicsCommandList* s_fitList = nullptr;
ID3D12Fence*               s_fitFence = nullptr;
UINT64                     s_fitVal = 0;
UINT64                     s_fitSlotFence[kFitRing] = {};
UINT                       s_fitIdx = 0, s_fitInc = 0;
bool                       s_fitInit = false, s_fitFailed = false;
float                      s_camT[3] = { 0, 0, 0 };
float                      s_camConf = 0.0f;

const char* kFitCS =
"Texture2D<float2> gMv    : register(t0);\n"
"Texture2D<float>  gDepth : register(t1);\n"
"RWByteAddressBuffer gOut : register(u0);\n"
"SamplerState gPt : register(s0);\n"
"cbuffer P : register(b0) {\n"
"    float gNear; float gFar; float gTanHalfV; float gAspect;\n"
"    float gNearCut; float gFarCut; float gYaw; float gPitch;\n"
"    float gMvScaleX; float gMvScaleY; float gMaxFlow; uint gGX;\n"
"    uint gGY; uint gPad0; uint gPad1; uint gPad2;\n"
"};\n"
"[numthreads(1,1,1)]\n"
"void CSMain() {\n"
"    float M00=0,M01=0,M02=0,M03=0,M04=0,M11=0,M12=0,M13=0,M14=0;\n"
"    float M22=0,M23=0,M24=0,M33=0,M34=0,M44=0, b0=0,b1=0,b2=0,b3=0,b4=0; uint cnt=0;\n"
"    float th = gTanHalfV; float tw = gTanHalfV * gAspect;\n"
"    float cy=cos(gYaw), sy=sin(gYaw), cp=cos(gPitch), sp=sin(gPitch);\n"
"    [loop] for (uint yy=0; yy<gGY; ++yy)\n"
"      [loop] for (uint xx=0; xx<gGX; ++xx) {\n"
"          float2 uv = float2((xx+0.5f)/gGX, (yy+0.5f)/gGY);\n"
"          float d = gDepth.SampleLevel(gPt, uv, 0);\n"
"          if (d > gNearCut) continue;        // near-field weapon: not world parallax\n"
"          if (d < gFarCut)  continue;        // sky / invalid\n"
"          // Reversed-Z, infinite far (CP2077 passes near=16000 far=0, which makes the near/far form\n"
"          // degenerate): Z is proportional to 1/d, so inverse depth w = 1/Z is proportional to d. The\n"
"          // proportionality constant just rescales T (absorbed by the strength), so near/far drop out.\n"
"          float w = max(d, 1e-4f);   // w = 1/Z (inverse depth), up to a constant\n"
"          float2 m = gMv.SampleLevel(gPt, uv, 0);\n"
"          float2 mvuv = float2(m.x*gMvScaleX, m.y*gMvScaleY);\n"
"          float nx = uv.x*2.0f-1.0f, ny = 1.0f - uv.y*2.0f;\n"
"          // Predicted rotational source displacement (same perspective rotation as mode 4).\n"
"          float3 dir = float3(nx*tw, ny*th, 1.0f);\n"
"          float3 dp  = float3(dir.x, cp*dir.y - sp*dir.z, sp*dir.y + cp*dir.z);\n"
"          float3 fr  = float3(cy*dp.x + sy*dp.z, dp.y, -sy*dp.x + cy*dp.z);\n"
"          float2 rotuv = (fr.z>1e-4f) ? float2((fr.x/fr.z/tw+1.0f)*0.5f, (1.0f - fr.y/fr.z/th)*0.5f) : uv;\n"
"          float2 res = mvuv - (rotuv - uv);  // residual = translation parallax + leftover rotation\n"
"          if (abs(res.x)+abs(res.y) > gMaxFlow) continue;   // reject movers / outliers\n"
"          // 5-DOF model: flowUV = w*(M*T) + U. T=(Tx,Ty,Tz) is depth-VARYING parallax; U=(Ux,Uy) is a\n"
"          // uniform nuisance flow that absorbs residual rotation / sens / MV-scale error (depth-flat),\n"
"          // so it can't contaminate T. Design rows ax,ay over unknowns [Tx,Ty,Tz,Ux,Uy].\n"
"          float ax0=w*-0.5f/tw, ax1=0.0f, ax2=w*0.5f*nx, ax3=1.0f, ax4=0.0f;\n"
"          float ay0=0.0f, ay1=w*0.5f/th, ay2=-w*0.5f*ny, ay3=0.0f, ay4=1.0f;\n"
"          float rx=res.x, ry=res.y;\n"
"          M00+=ax0*ax0+ay0*ay0; M01+=ax0*ax1+ay0*ay1; M02+=ax0*ax2+ay0*ay2; M03+=ax0*ax3+ay0*ay3; M04+=ax0*ax4+ay0*ay4;\n"
"          M11+=ax1*ax1+ay1*ay1; M12+=ax1*ax2+ay1*ay2; M13+=ax1*ax3+ay1*ay3; M14+=ax1*ax4+ay1*ay4;\n"
"          M22+=ax2*ax2+ay2*ay2; M23+=ax2*ax3+ay2*ay3; M24+=ax2*ax4+ay2*ay4;\n"
"          M33+=ax3*ax3+ay3*ay3; M34+=ax3*ax4+ay3*ay4; M44+=ax4*ax4+ay4*ay4;\n"
"          b0+=ax0*rx+ay0*ry; b1+=ax1*rx+ay1*ry; b2+=ax2*rx+ay2*ry; b3+=ax3*rx+ay3*ry; b4+=ax4*rx+ay4*ry;\n"
"          cnt++;\n"
"      }\n"
"    gOut.Store(0, asuint(M00)); gOut.Store(4, asuint(M01)); gOut.Store(8, asuint(M02)); gOut.Store(12,asuint(M03)); gOut.Store(16,asuint(M04));\n"
"    gOut.Store(20,asuint(M11)); gOut.Store(24,asuint(M12)); gOut.Store(28,asuint(M13)); gOut.Store(32,asuint(M14));\n"
"    gOut.Store(36,asuint(M22)); gOut.Store(40,asuint(M23)); gOut.Store(44,asuint(M24));\n"
"    gOut.Store(48,asuint(M33)); gOut.Store(52,asuint(M34)); gOut.Store(56,asuint(M44));\n"
"    gOut.Store(60,asuint(b0)); gOut.Store(64,asuint(b1)); gOut.Store(68,asuint(b2)); gOut.Store(72,asuint(b3)); gOut.Store(76,asuint(b4));\n"
"    gOut.Store(80, cnt);\n"
"}\n";

// Solve a 5x5 linear system A x = b by Gaussian elimination with partial pivoting (A is overwritten).
bool Solve5(double A[5][5], double b[5], double x[5]) {
    for (int c = 0; c < 5; ++c) {
        int piv = c; double best = fabs(A[c][c]);
        for (int r = c + 1; r < 5; ++r) { double v = fabs(A[r][c]); if (v > best) { best = v; piv = r; } }
        if (best < 1e-15) return false;
        if (piv != c) {
            for (int k = 0; k < 5; ++k) { double t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
            double t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < 5; ++r) {
            double f = A[r][c] / A[c][c];
            for (int k = c; k < 5; ++k) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 4; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < 5; ++k) s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return true;
}

bool BuildFitPipeline() {
    if (s_fitInit) return true;
    if (s_fitFailed || !s_device) return false;

    D3D12_DESCRIPTOR_RANGE rSrv = {}; rSrv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rSrv.NumDescriptors = 2; rSrv.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE rUav = {}; rUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rUav.NumDescriptors = 1; rUav.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER p[3] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[0].DescriptorTable.NumDescriptorRanges = 1; p[0].DescriptorTable.pDescriptorRanges = &rSrv;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &rUav;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[2].Constants.ShaderRegister = 0; p[2].Constants.Num32BitValues = 16;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 3; rs.pParameters = p; rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { if (sig) sig->Release(); if (err) err->Release(); s_fitFailed = true; return false; }
    HRESULT hr = s_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s_fitRoot));
    sig->Release(); if (err) err->Release();
    if (FAILED(hr)) { s_fitFailed = true; return false; }
    ID3DBlob* cs = nullptr; ID3DBlob* e1 = nullptr;
    if (FAILED(D3DCompile(kFitCS, strlen(kFitCS), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &cs, &e1))) {
        LOG_ERROR("DepthCapture: fit CS compile failed: %s", e1 ? (char*)e1->GetBufferPointer() : "?");
        if (cs) cs->Release(); if (e1) e1->Release(); s_fitFailed = true; return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {}; cp.pRootSignature = s_fitRoot; cp.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    hr = s_device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&s_fitPso)); cs->Release(); if (e1) e1->Release();
    if (FAILED(hr)) { s_fitFailed = true; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_fitHeap)))) { s_fitFailed = true; return false; }
    s_fitInc = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES dh = {}; dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kFitU * sizeof(UINT); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(s_device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_fitAccum)))) { s_fitFailed = true; return false; }
    D3D12_HEAP_PROPERTIES rh = {}; rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (UINT64)kFitU * sizeof(UINT) * kFitRing; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_fitReadback)))) { s_fitFailed = true; return false; }
    D3D12_RANGE none = { 0, 0 }; s_fitReadback->Map(0, &none, (void**)&s_fitMapped);

    for (UINT i = 0; i < kFitRing; ++i)
        if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_fitAlloc[i])))) { s_fitFailed = true; return false; }
    if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_fitAlloc[0], nullptr, IID_PPV_ARGS(&s_fitList)))) { s_fitFailed = true; return false; }
    s_fitList->Close();
    s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fitFence));
    s_fitInit = true;
    LOG_INFO("DepthCapture: camera-translation fit pipeline built");
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

bool GetMvSRV(ID3D12Resource** tex, DXGI_FORMAT* srvFmt) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_mvCopy) return false;
    if (tex)    *tex    = s_mvCopy;
    if (srvFmt) *srvFmt = s_mvSrvFmt;
    return true;
}

void ComputeCameraTranslation(ID3D12CommandQueue* queue, float yawDelta, float pitchDelta, float nearCut) {
    ID3D12Resource* mvTex = nullptr; ID3D12Resource* depthTex = nullptr;
    DXGI_FORMAT mvFmt = DXGI_FORMAT_UNKNOWN, dFmt = DXGI_FORMAT_UNKNOWN; Cam cam;
    { std::lock_guard<std::mutex> lk(s_mtx);
      mvTex = s_mvCopy; depthTex = s_depthCopy; mvFmt = s_mvSrvFmt; dFmt = s_srvFmt; cam = s_cam; }
    if (!queue || !mvTex || !depthTex) return;
    if (cam.fovV <= 0.01f || cam.renderH == 0) return;
    if (!BuildFitPipeline()) return;

    UINT slot = s_fitIdx % kFitRing;
    // Read the result from kFitRing dispatches ago (complete now) and solve the 3x3 normal equations.
    if (s_fitSlotFence[slot] != 0 && s_fitFence->GetCompletedValue() >= s_fitSlotFence[slot]) {
        const UINT* r = &s_fitMapped[slot * kFitU];
        auto F = [&](int i){ float f; UINT u = r[i]; memcpy(&f, &u, 4); return (double)f; };
        // Unpack the 5x5 upper triangle (15) into a full symmetric matrix, then b (5) and cnt.
        double A[5][5]; double b[5];
        { int k = 0; for (int i = 0; i < 5; ++i) for (int j = i; j < 5; ++j) { double v = F(k++); A[i][j] = v; A[j][i] = v; } }
        for (int i = 0; i < 5; ++i) b[i] = F(15 + i);
        UINT cnt = r[20];
        // Tikhonov regularization on the diagonal: stabilizes a poorly-observed DOF (near-zero motion,
        // or a move that excites only one axis) so the solve can't blow up.
        double tr = A[0][0] + A[1][1] + A[2][2];
        double lam = 1e-3 * (tr / 3.0) + 1e-9;
        A[0][0] += lam; A[1][1] += lam; A[2][2] += lam;
        double x[5];
        if (cnt > 60 && Solve5(A, b, x)) {
            float conf = (float)cnt / (float)(kFitGX * kFitGY);
            const float a = 0.18f;   // EMA: translation is a per-game-frame velocity; smooth out fit noise
            // MV is a source displacement (old-new) = -(forward flow), so the solve recovers -T. Negate
            // to report the physical camera translation (x=right, y=up, z=forward all positive).
            s_camT[0] = s_camT[0]*(1-a) + (float)(-x[0])*a;
            s_camT[1] = s_camT[1]*(1-a) + (float)(-x[1])*a;
            s_camT[2] = s_camT[2]*(1-a) + (float)(-x[2])*a;
            s_camConf = s_camConf*(1-a) + conf*a;
            static int dbg = 0;
            if ((dbg++ % 120) == 0)
                LOG_INFO("DepthCapture: camT=(%.4f,%.4f,%.4f) inliers=%u/%u conf=%.2f",
                         s_camT[0], s_camT[1], s_camT[2], cnt, kFitGX * kFitGY, s_camConf);
        }
    }

    // Descriptors: [0]=mv SRV (t0), [1]=depth SRV (t1), [2]=accum UAV (u0).
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_fitHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_fitHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = mvFmt; s_device->CreateShaderResourceView(mvTex, &sv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE dCpu = cpu; dCpu.ptr += s_fitInc;
    sv.Format = dFmt; s_device->CreateShaderResourceView(depthTex, &sv, dCpu);
    D3D12_CPU_DESCRIPTOR_HANDLE uCpu = cpu; uCpu.ptr += 2 * s_fitInc;
    D3D12_GPU_DESCRIPTOR_HANDLE uGpu = gpu; uGpu.ptr += 2 * s_fitInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.FirstElement = 0; uav.Buffer.NumElements = kFitU; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    s_device->CreateUnorderedAccessView(s_fitAccum, nullptr, &uav, uCpu);

    s_fitAlloc[slot]->Reset();
    s_fitList->Reset(s_fitAlloc[slot], s_fitPso);
    Barrier(s_fitList, s_fitAccum, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s_fitList->SetComputeRootSignature(s_fitRoot);
    ID3D12DescriptorHeap* heaps[] = { s_fitHeap };
    s_fitList->SetDescriptorHeaps(1, heaps);
    s_fitList->SetComputeRootDescriptorTable(0, gpu);     // SRVs (mv, depth)
    s_fitList->SetComputeRootDescriptorTable(1, uGpu);    // UAV (accum)
    float aspect = (float)cam.renderW / (float)cam.renderH;
    float tanHalfV = tanf(cam.fovV * 0.5f);
    struct { float nearZ, farZ, tanHalfV, aspect, nearCut, farCut, yaw, pitch, mvsx, mvsy, maxFlow;
             UINT gx, gy, p0, p1, p2; } c =
        { cam.nearZ, cam.farZ, tanHalfV, aspect, nearCut, 1e-5f, yawDelta, pitchDelta,
          1.0f, 1.0f, 0.10f, kFitGX, kFitGY, 0, 0, 0 };
    s_fitList->SetComputeRoot32BitConstants(2, 16, &c, 0);
    s_fitList->Dispatch(1, 1, 1);
    Barrier(s_fitList, s_fitAccum, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s_fitList->CopyBufferRegion(s_fitReadback, (UINT64)slot * kFitU * sizeof(UINT), s_fitAccum, 0, kFitU * sizeof(UINT));
    Barrier(s_fitList, s_fitAccum, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    s_fitList->Close();
    ID3D12CommandList* lists[] = { s_fitList };
    queue->ExecuteCommandLists(1, lists);
    s_fitSlotFence[slot] = ++s_fitVal;
    queue->Signal(s_fitFence, s_fitVal);
    s_fitIdx++;
}

void GetCameraTranslation(float out3[3], float* confidence) {
    // Soft deadzone: the fit floats around ~0.1-0.5 (and rarely spikes) when the camera is actually
    // still. Subtracting a threshold zeroes that standing jitter (so the warp doesn't swim in place)
    // while real motion (walk ~2, sprint ~5) passes through nearly untouched.
    const float dz = 0.30f;
    auto soft = [](float v, float t){ return v > t ? v - t : (v < -t ? v + t : 0.0f); };
    if (out3) { out3[0] = soft(s_camT[0], dz); out3[1] = soft(s_camT[1], dz); out3[2] = soft(s_camT[2], dz); }
    if (confidence) *confidence = s_camConf;
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
      if (s_mvCopy)    { s_mvCopy->Release();    s_mvCopy = nullptr; }
      if (s_device)    { s_device->Release();    s_device = nullptr; } }
}

} // namespace DepthCapture
