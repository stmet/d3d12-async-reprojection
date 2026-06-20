#include "rt_tracker.h"
#include "../common/logger.h"
#include <detours.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace RtTracker {
namespace {

// ---- config / swapchain signature ------------------------------------------------------------
bool        s_enabled = false;
bool        s_enabledChecked = false;
uint32_t    s_scW = 0, s_scH = 0;
DXGI_FORMAT s_scFmt = DXGI_FORMAT_UNKNOWN;

// ---- hook state ------------------------------------------------------------------------------
typedef void (STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*,
              const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList*, UINT,
              const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);

PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
PFN_OMSetRenderTargets     o_OMSetRenderTargets     = nullptr;

ID3D12Device* s_device = nullptr;
bool          s_hooksInstalled = false;
std::mutex    s_installMutex;

// ---- maps ------------------------------------------------------------------------------------
std::mutex s_mapMutex;                                       // guards the three maps below
std::unordered_map<SIZE_T, ID3D12Resource*> s_handleToRes;  // RTV cpu-handle.ptr -> resource
std::unordered_map<ID3D12GraphicsCommandList*, ID3D12Resource*> s_lastBound; // per-list last candidate
std::unordered_set<ID3D12Resource*> s_excluded;             // replacement + real backbuffers

// ---- hud-less destination --------------------------------------------------------------------
std::mutex      s_capMutex;
ID3D12Resource* s_hudless = nullptr;
uint32_t        s_hudlessW = 0, s_hudlessH = 0;
DXGI_FORMAT     s_hudlessFmt = DXGI_FORMAT_UNKNOWN;

// ---- capture point -----------------------------------------------------------------------------
int  s_capturePoint = 0;          // 0 = first-after-upscale (default), 1 = last-before-present
bool s_armed = false;             // set by OnUpscaleDispatch, consumed by the first capture (mode 0)

// ---- stats -----------------------------------------------------------------------------------
Stats s_stats;
uint32_t s_candidatesThisFrame = 0;

// DXGI format "copy family": CopyResource needs src/dst in the same family (bitwise copy). Coarse
// grouping is enough to recognise a backbuffer-compatible color target.
int FormatGroup(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT: return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT: return 3;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: return 4;
        case DXGI_FORMAT_R11G11B10_FLOAT: return 5;
        default: return 0;
    }
}

// Diagnostic: does this resource match the swapchain signature (dims+format), IGNORING the exclusion
// set? Used by the bind/upscale logging to reveal the full pipeline (incl. excluded backbuffers).
int s_logBudget = 0;
bool MatchesSwapchain(ID3D12Resource* res, D3D12_RESOURCE_DESC& d) {
    if (!res || s_scW == 0) return false;
    d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    uint32_t tolX = s_scW / 8, tolY = s_scH / 8;
    if (d.Width  + tolX < s_scW || d.Width  > s_scW + tolX) return false;
    if (d.Height + tolY < s_scH || d.Height > s_scH + tolY) return false;
    int g = FormatGroup(s_scFmt);
    return g != 0 && FormatGroup(d.Format) == g;
}

bool IsCandidate(ID3D12Resource* res) {
    if (!res || s_scW == 0) return false;
    {
        // exclusion set (replacement + real backbuffers + our own targets)
        if (s_excluded.find(res) != s_excluded.end()) return false;
    }
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                   D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE |
                   D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return false;
    // dimensions: exact (±1/8 tolerance for slightly-padded scene buffers)
    uint32_t tolX = s_scW / 8, tolY = s_scH / 8;
    if (d.Width  + tolX < s_scW || d.Width  > s_scW + tolX) return false;
    if (d.Height + tolY < s_scH || d.Height > s_scH + tolY) return false;
    // format family must match the swapchain (so CopyResource into our hud-less tex is valid)
    int g = FormatGroup(s_scFmt);
    if (g == 0 || FormatGroup(d.Format) != g) return false;
    return true;
}

bool EnsureHudless(ID3D12Resource* like) {
    std::lock_guard<std::mutex> lk(s_capMutex);
    if (s_hudless) return true;
    if (!s_device) return false;
    D3D12_RESOURCE_DESC d = like->GetDesc();
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = d.Width; rd.Height = d.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = s_scFmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // SHARED + SIMULTANEOUS_ACCESS: the game's graphics queue writes (copy) while the presenter/overlay
    // queue samples it — same cross-queue contract as the other export textures (must keep SHARED).
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    HRESULT hr = s_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s_hudless));
    if (FAILED(hr)) {
        LOG_ERROR("RtTracker: hud-less texture create failed 0x%08X (%ux%u fmt=%u)",
                  (unsigned)hr, (unsigned)d.Width, (unsigned)d.Height, (unsigned)s_scFmt);
        s_hudless = nullptr;
        return false;
    }
    s_hudlessW = (uint32_t)d.Width; s_hudlessH = d.Height; s_hudlessFmt = s_scFmt;
    LOG_INFO("RtTracker: hud-less texture created %ux%u fmt=%u", s_hudlessW, s_hudlessH, (unsigned)s_scFmt);
    return true;
}

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

// Record (on the game's command list) a copy of `src` (a just-unbound RTV, so in RENDER_TARGET state)
// into our hud-less texture. The dest is COMMON + simultaneous-access -> implicit COPY_DEST promotion.
void CaptureHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* src) {
    if (!EnsureHudless(src) || !s_hudless) return;
    Barrier(cl, src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(s_hudless, src);
    Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    s_stats.captures++;
}

// ---- hooks -----------------------------------------------------------------------------------
void STDMETHODCALLTYPE hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource,
        const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Dest) {
    o_CreateRenderTargetView(This, pResource, pDesc, Dest);
    if (pResource) {
        std::lock_guard<std::mutex> lk(s_mapMutex);
        s_handleToRes[Dest.ptr] = pResource;
    }
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRTV,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRTVs, BOOL SingleHandleRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDSV) {
    ID3D12Resource* newRes = nullptr;
    if (NumRTV >= 1 && pRTVs) {
        std::lock_guard<std::mutex> lk(s_mapMutex);
        auto it = s_handleToRes.find(pRTVs[0].ptr);     // first bound RT
        if (it != s_handleToRes.end()) newRes = it->second;
    }

    // Diagnostic: log every display-res RT bound (including EXCLUDED backbuffers) so we can see the
    // real pipeline order vs the upscale, and whether the clean scene lands in an excluded buffer.
    if (s_logBudget > 0 && newRes) {
        D3D12_RESOURCE_DESC dd;
        if (MatchesSwapchain(newRes, dd)) {
            bool excl;
            { std::lock_guard<std::mutex> lk(s_mapMutex); excl = s_excluded.count(newRes) > 0; }
            LOG_INFO("RTbind cl=%p res=%p %llux%u fmt=%u %s armed=%d",
                     (void*)This, (void*)newRes, (unsigned long long)dd.Width, dd.Height,
                     (unsigned)dd.Format, excl ? "EXCLUDED" : "candidate", (int)s_armed);
            s_logBudget--;
        }
    }

    ID3D12Resource* prev = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mapMutex);
        auto it = s_lastBound.find(This);
        if (it != s_lastBound.end()) prev = it->second;
        s_lastBound[This] = IsCandidate(newRes) ? newRes : nullptr;
    }

    // We copy a candidate's content BEFORE the original OMSet, so the copy precedes that pass's draws.
    // Cyberpunk draws the main HUD early and only the crosshair last, all into one display buffer, so:
    //   0 = FIRST candidate bind after the upscale -> pre-content = post-FX scene, BEFORE any HUD
    //       (armed by OnUpscaleDispatch; the true hud-less candidate). Default.
    //   1 = LAST candidate bind before present -> pre-content = scene + HUD minus the crosshair.
    //   2 = LAST candidate unbind before present -> post-pass content = scene + full HUD.
    ID3D12Resource* cap = nullptr;
    if (s_capturePoint == 0) {
        if (s_armed && IsCandidate(newRes)) { cap = newRes; s_armed = false; }
    } else if (s_capturePoint == 1) {
        if (IsCandidate(newRes)) cap = newRes;
    } else {
        if (prev && prev != newRes && IsCandidate(prev)) cap = prev;
    }
    if (cap) {
        s_candidatesThisFrame++;
        D3D12_RESOURCE_DESC d = cap->GetDesc();
        s_stats.lastCandidateW = (uint32_t)d.Width; s_stats.lastCandidateH = d.Height;
        s_stats.lastCandidateFmt = (uint32_t)d.Format;
        CaptureHudless(This, cap);
    }

    o_OMSetRenderTargets(This, NumRTV, pRTVs, SingleHandleRange, pDSV);
}

} // namespace

bool Enabled() {
    if (!s_enabledChecked) {
        s_enabled = (GetEnvironmentVariableW(L"ASYNCREPROJ_HUDLESS", nullptr, 0) != 0);
        s_enabledChecked = true;
    }
    return s_enabled;
}

void SetSwapchainInfo(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (!Enabled()) return;
    s_scW = w; s_scH = h; s_scFmt = fmt;
}

void RegisterExcludedTarget(ID3D12Resource* buf) {
    if (!Enabled() || !buf) return;
    std::lock_guard<std::mutex> lk(s_mapMutex);
    s_excluded.insert(buf);
}

void ClearExcludedTargets() {
    std::lock_guard<std::mutex> lk(s_mapMutex);
    s_excluded.clear();
}

void EnsureHooks(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList) {
    if (!Enabled() || !device || !cmdList) return;
    std::lock_guard<std::mutex> lk(s_installMutex);
    if (s_hooksInstalled) return;

    s_device = device;
    void** devVtbl = *(void***)device;
    void** clVtbl  = *(void***)cmdList;
    o_CreateRenderTargetView = (PFN_CreateRenderTargetView)devVtbl[20];
    o_OMSetRenderTargets     = (PFN_OMSetRenderTargets)clVtbl[46];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o_CreateRenderTargetView, hkCreateRenderTargetView);
    DetourAttach(&(PVOID&)o_OMSetRenderTargets, hkOMSetRenderTargets);
    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        s_hooksInstalled = true;
        s_stats.hooksInstalled = true;
        s_logBudget = 160;   // dump the first few frames of display-res RT binds, then go quiet
        LOG_INFO("RtTracker: hud-less hooks installed (CreateRTV + OMSetRenderTargets), sc %ux%u fmt=%u",
                 s_scW, s_scH, (unsigned)s_scFmt);
    } else {
        LOG_ERROR("RtTracker: DetourTransactionCommit failed %ld", err);
        o_CreateRenderTargetView = nullptr;
        o_OMSetRenderTargets = nullptr;
    }
}

void OnUpscaleDispatch() {
    if (!s_enabled) return;
    s_armed = true;
    if (s_logBudget > 0) LOG_INFO("RTbind ---- upscale dispatch (frame boundary) ----");
}
void SetCapturePoint(int p) { s_capturePoint = (p < 0) ? 0 : (p > 2 ? 2 : p); }
int  GetCapturePoint()      { return s_capturePoint; }

ID3D12Resource* GetHudless()       { return s_hudless; }
DXGI_FORMAT     GetHudlessFormat() { return s_hudlessFmt; }
void GetHudlessSize(uint32_t& w, uint32_t& h) { w = s_hudlessW; h = s_hudlessH; }

Stats GetStats() {
    {
        std::lock_guard<std::mutex> lk(s_mapMutex);
        s_stats.rtvMapSize = (uint32_t)s_handleToRes.size();
    }
    s_stats.candidatesLastFrame = s_candidatesThisFrame;
    s_candidatesThisFrame = 0;   // reset each time the overlay polls (~per present)
    return s_stats;
}

} // namespace RtTracker
