#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>

struct ExportSlot {
    ID3D12Resource* colorTex = nullptr;
    ID3D12Resource* depthTex = nullptr;
    ID3D12Resource* mvTex = nullptr;
    ID3D12Resource* fgHudlessTex = nullptr;
    ID3D12Resource* fgUiTex = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t currentFenceValue = 0;
    HANDLE fenceEvent = nullptr;

    bool depthValid = false;
    bool mvValid = false;
    bool fgHudlessValid = false;

    // Diagnostic: m_sequenceNumber when each was last written, to detect color/hudless frame skew
    // (the "world-edge outline under motion" = the two buffers are a frame apart).
    uint64_t colorSeq = 0;
    uint64_t hudlessSeq = 0;
};

// In-process snapshot of the latest fully-published capture slot, for the overlay's debug views
// (P1). Returns the textures (in COMMON / simultaneous-access, sample directly via SRV) plus the
// SRV-ready formats and reprojection params — the presenter's live source for the captured
// depth/MV/hudless/color it reprojects each frame.
struct DebugCapture {
    ID3D12Resource* color = nullptr;  DXGI_FORMAT colorSrvFmt = DXGI_FORMAT_UNKNOWN;  uint32_t colorW = 0, colorH = 0;
    ID3D12Resource* depth = nullptr;  DXGI_FORMAT depthSrvFmt = DXGI_FORMAT_UNKNOWN;  uint32_t depthW = 0, depthH = 0;
    ID3D12Resource* mv    = nullptr;  DXGI_FORMAT mvSrvFmt    = DXGI_FORMAT_UNKNOWN;  uint32_t mvW = 0,    mvH = 0;
    ID3D12Resource* fgHudless = nullptr; DXGI_FORMAT fgHudlessSrvFmt = DXGI_FORMAT_UNKNOWN; uint32_t fgHudlessW = 0, fgHudlessH = 0;
    ID3D12Resource* fgUi = nullptr;      DXGI_FORMAT fgUiSrvFmt = DXGI_FORMAT_UNKNOWN;      uint32_t fgUiW = 0,      fgUiH = 0;
    uint32_t validity = 0;  uint64_t seq = 0;
    float mvScaleX = 0, mvScaleY = 0, camNear = 0, camFar = 0, camFovV = 0;
    uint32_t renderW = 0, renderH = 0;
};

class ExportManager {
public:
    static ExportManager& Instance();

    void SetDevice(ID3D12Device* device);
    void SetActiveCommandQueue(ID3D12CommandQueue* queue);
    void InitializeSwapChain(IDXGISwapChain* swapchain);

    // Called from the FSR3 dispatch hook with the game's own command list.
    // Copies depth/MV into the current export slot inline. This is the ONLY point
    // where these engine resources have a known, deterministic state (FSR's input
    // contract = shader-read), and recording onto the game's own list guarantees
    // correct GPU-timeline ordering. Color is captured separately at Present from
    // the backbuffer (whose state at Present is deterministically PRESENT).
    void CopyDepthMVOnDispatch(ID3D12GraphicsCommandList* cmdList,
                               ID3D12Resource* depth,
                               ID3D12Resource* mv);

    void CopyFgHudlessOnDispatch(ID3D12GraphicsCommandList* cmdList,
                                 ID3D12Resource* fgHudless,
                                 uint32_t ffxState);

    bool OnPresent(IDXGISwapChain* swapchain);

    // v3: called from the FSR2/FSR3 dispatch hook with reprojection params read from the
    // dispatch description (motionVectorScale, render res, jitter, camera). Cached and
    // surfaced via GetDebugCapture so the warp can reproject without guesswork.
    void SetUpscalerParams(float mvScaleX, float mvScaleY, float jitterX, float jitterY,
                           uint32_t renderW, uint32_t renderH,
                           float camNear, float camFar, float camFovV);

    // Fills `out` with the latest fully-published capture slot for the overlay debug views.
    // Returns false if nothing has been captured yet. Thread-safe.
    bool GetDebugCapture(DebugCapture& out);

    // Gain auto-calibration: returns the average motion-vector at the 4 frame corners (far-field =
    // pure camera screen motion) in UV space, ~1-2 frames stale. False if no MV captured yet or the
    // value looks like garbage. Drives the mouse-counts -> screen-shift regression in WarpRenderer.
    bool ReadCornerMV(float& uvU, float& uvV);

private:
    ExportManager();
    ~ExportManager();

    bool RecreateExportTextures(
        uint32_t colorWidth, uint32_t colorHeight, DXGI_FORMAT colorFmt, D3D12_RESOURCE_FLAGS colorFlags,
        uint32_t depthWidth, uint32_t depthHeight, DXGI_FORMAT depthFmt, D3D12_RESOURCE_FLAGS depthFlags,
        uint32_t mvWidth, uint32_t mvHeight, DXGI_FORMAT mvFmt, D3D12_RESOURCE_FLAGS mvFlags);
    void ReleaseTextures();
    void CopyResourceToExport(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* src, ID3D12Resource* dst);

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;           // game's present queue (borrowed, not owned)

    // Dedicated COPY queue + triple-buffered allocators for the color capture. The copy runs
    // on the GPU's DMA engine in parallel with the game's flip and next-frame rendering,
    // instead of serializing in front of the flip on the present queue. m_copyOrderFence is
    // signaled on the present queue and waited on by the copy queue to order the capture
    // after the game's frame rendering without blocking the flip.
    ID3D12CommandQueue* m_copyQueue = nullptr;
    ID3D12CommandAllocator* m_copyAllocators[3] = { nullptr, nullptr, nullptr };
    ID3D12GraphicsCommandList* m_copyList = nullptr;
    ID3D12Fence* m_copyOrderFence = nullptr;
    uint64_t m_copyOrderValue = 0;

    // Cached QI of the swapchain to IDXGISwapChain3 (for GetCurrentBackBufferIndex), keyed on
    // the raw swapchain pointer so we re-QI only when the swapchain object actually changes.
    IDXGISwapChain* m_cachedSwapchainRaw = nullptr;  // identity comparison only — never dereferenced
    IDXGISwapChain3* m_cachedSc3 = nullptr;

    uint32_t m_colorWidth = 0;
    uint32_t m_colorHeight = 0;
    uint32_t m_depthWidth = 0;
    uint32_t m_depthHeight = 0;
    uint32_t m_mvWidth = 0;
    uint32_t m_mvHeight = 0;
    DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_mvFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_FLAGS m_colorFlags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_FLAGS m_depthFlags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_FLAGS m_mvFlags = D3D12_RESOURCE_FLAG_NONE;

    uint64_t m_frameId = 0;
    uint64_t m_sequenceNumber = 0;
    uint32_t m_presentsSinceDispatch = 0;

    // Sticky FSR3 resource parameters — updated when intercepted, held across FSR3-inactive frames
    // so RecreateExportTextures never ping-pongs between real and default formats.
    DXGI_FORMAT          m_fsr3DepthFmt    = DXGI_FORMAT_D32_FLOAT;
    D3D12_RESOURCE_FLAGS m_fsr3DepthFlags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    uint32_t             m_fsr3DepthWidth  = 0;
    uint32_t             m_fsr3DepthHeight = 0;
    DXGI_FORMAT          m_fsr3MVFmt       = DXGI_FORMAT_R16G16_FLOAT;
    D3D12_RESOURCE_FLAGS m_fsr3MVFlags     = D3D12_RESOURCE_FLAG_NONE;
    uint32_t             m_fsr3MVWidth     = 0;
    uint32_t             m_fsr3MVHeight    = 0;

    // Cached FG HUDLessColor format — updated in CopyFgHudlessOnDispatch when we first see
    // the real resource. RecreateExportTextures uses this to create fgHudlessTex with the
    // correct format (typically R10G10B10A2_UNORM or R16G16B16A16_FLOAT, NOT R8G8B8A8_UNORM).
    DXGI_FORMAT          m_fgHudlessFmt    = DXGI_FORMAT_UNKNOWN;
    uint32_t             m_fgHudlessWidth  = 0;
    uint32_t             m_fgHudlessHeight = 0;
    DXGI_FORMAT          m_fgHudlessExportFmt = DXGI_FORMAT_UNKNOWN; // current format used for fgHudlessTex creation

    // Set by CopyDepthMVOnDispatch each frame; consumed + reset by OnPresent when it
    // publishes the slot's metadata. Tells OnPresent whether depth/MV were captured
    // this frame (so it can set the validity bits) and which slot they went into.
    bool m_dispatchDepthValid = false;
    bool m_dispatchMVValid = false;
    bool m_dispatchFgHudlessValid = false;
    bool m_dispatchOccurred = false;
    uint32_t m_dispatchSlot = 0;

    // Gain-calibration MV-corner readback (4 corners x 256-byte-aligned rows = 1 KB), written on the
    // game's command list during the dispatch copy, persistently mapped, read by ReadCornerMV.
    ID3D12Resource* m_mvCornerReadback = nullptr;
    void*           m_mvCornerMapped = nullptr;
    float           m_calMvScaleX = 1.0f, m_calMvScaleY = 1.0f;
    bool            m_mvCornerValid = false;

    // v3 reprojection params, latest values seen from the FSR dispatch hook. Published into
    // slot metadata at Present. Guarded by m_mutex.
    float    m_mvScaleX = 0.0f, m_mvScaleY = 0.0f;
    float    m_jitterX = 0.0f,  m_jitterY = 0.0f;
    uint32_t m_renderW = 0,     m_renderH = 0;
    float    m_camNear = 0.0f,  m_camFar = 0.0f, m_camFovV = 0.0f;
    bool     m_upscalerParamsValid = false;

    // Deferred publish: a slot is promoted to the in-process debug views one frame after its
    // copies are issued, so the depth/MV copy (on the game's async FSR queue) is guaranteed
    // complete before the presenter samples it. Holds the previous frame's slot + validity.
    uint32_t     m_pendingValidity = 0;
    uint64_t     m_pendingSeq = 0;
    uint32_t     m_pendingSlot = 0;
    bool         m_pendingValid = false;

    // Tracks the slot most recently advertised (deferred-publish), so the overlay's in-process
    // debug views read a slot whose GPU copies are guaranteed complete (a frame old).
    uint32_t     m_lastPubSlot = 0;
    uint32_t     m_lastPubValidity = 0;
    uint64_t     m_lastPubSeq = 0;
    bool         m_lastPubValid = false;

    // Last slot whose color-copy fence was confirmed complete — what the debug views actually
    // sample, so they never read a texture mid-copy (the flickering-black-dots artifact).
    uint32_t     m_debugReadySlot = 0;
    bool         m_debugReadyValid = false;

    ExportSlot m_slots[3];

    std::mutex m_mutex;
    bool m_texturesInitialized = false;
};
