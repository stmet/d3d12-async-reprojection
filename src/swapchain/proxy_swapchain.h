#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

// Pass-through proxy swapchain (P0.2).
//
// Wraps the game's real IDXGISwapChain4 and forwards every call 1:1. The only methods that do
// extra work are Present / Present1, which run our present-time hooks (frame capture + overlay)
// and then forward to the real swapchain via the un-hooked trampoline. This is the object that
// later phases extend to own present timing (presenter thread) and insert reprojected frames.
//
// In this phase it is purely transparent — real frames are presented untouched. It exists now to
// de-risk the wrapper mechanics (QueryInterface identity, GetBuffer, ResizeBuffers, fullscreen
// transitions) in isolation, before any threading or pacing is layered on top.
//
// Construction transfers one reference of `real` into the proxy (the proxy releases it on destroy).
//
// In async mode (ASYNCREPROJ_ASYNC=1) the proxy switches to the FFX replacement-buffer model: it
// allocates its own off-screen backbuffers and hands them to the game via GetBuffer, so the game
// renders into our textures, not the real swapchain. Present then hands the finished buffer to the
// presenter thread (which owns the real swapchain's presents) and returns immediately, instead of
// presenting on the game thread. In sync mode (default) the proxy is the transparent pass-through
// that runs the present-time warp inline.
class ProxySwapChain : public IDXGISwapChain4 {
public:
    explicit ProxySwapChain(IDXGISwapChain4* real) : m_real(real), m_ref(1) {}
    // Async-mode constructor: also captures the device + game queue and allocates replacement buffers.
    ProxySwapChain(IDXGISwapChain4* real, ID3D12Device* device, ID3D12CommandQueue* gameQueue);

    IDXGISwapChain4* Real() const { return m_real; }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG   STDMETHODCALLTYPE Release() override {
        ULONG c = (ULONG)InterlockedDecrement(&m_ref);
        if (c == 0) { m_real->Release(); delete this; }
        return c;
    }

    // ---- IDXGIObject ----
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_real->SetPrivateData(Name, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_real->SetPrivateDataInterface(Name, pUnknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_real->GetPrivateData(Name, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return m_real->GetParent(riid, ppParent);
    }

    // ---- IDXGIDeviceSubObject ----
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override {
        return m_real->GetDevice(riid, ppDevice);
    }

    // ---- IDXGISwapChain ----
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;          // (cpp)
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;  // (cpp)
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override {
        return m_real->SetFullscreenState(Fullscreen, pTarget);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override {
        return m_real->GetFullscreenState(pFullscreen, ppTarget);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override {
        return m_real->GetDesc(pDesc);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override; // (cpp)
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override {
        return m_real->ResizeTarget(pNewTargetParameters);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override {
        return m_real->GetContainingOutput(ppOutput);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        return m_real->GetFrameStatistics(pStats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override {
        return m_real->GetLastPresentCount(pLastPresentCount);
    }

    // ---- IDXGISwapChain1 ----
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override {
        return m_real->GetDesc1(pDesc);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override {
        return m_real->GetFullscreenDesc(pDesc);
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override {
        return m_real->GetHwnd(pHwnd);
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override {
        return m_real->GetCoreWindow(refiid, ppUnk);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) override; // (cpp)
    BOOL    STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
        return m_real->IsTemporaryMonoSupported();
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override {
        return m_real->GetRestrictToOutput(ppRestrictToOutput);
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override {
        return m_real->SetBackgroundColor(pColor);
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override {
        return m_real->GetBackgroundColor(pColor);
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {
        return m_real->SetRotation(Rotation);
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override {
        return m_real->GetRotation(pRotation);
    }

    // ---- IDXGISwapChain2 ----
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override {
        return m_real->SetSourceSize(Width, Height);
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override {
        return m_real->GetSourceSize(pWidth, pHeight);
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
        return m_real->SetMaximumFrameLatency(MaxLatency);
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
        return m_real->GetMaximumFrameLatency(pMaxLatency);
    }
    HANDLE  STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        return m_real->GetFrameLatencyWaitableObject();
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override {
        return m_real->SetMatrixTransform(pMatrix);
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override {
        return m_real->GetMatrixTransform(pMatrix);
    }

    // ---- IDXGISwapChain3 ----
    UINT    STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;   // (cpp)
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override {
        return m_real->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
        return m_real->SetColorSpace1(ColorSpace);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) override {
        return m_real->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    }

    // ---- IDXGISwapChain4 ----
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override {
        return m_real->SetHDRMetaData(Type, Size, pMetaData);
    }

private:
    ~ProxySwapChain();

    // Allocate / release the replacement backbuffers from the real swapchain's current desc.
    void AllocReplacementBuffers();
    void ReleaseReplacementBuffers();

    IDXGISwapChain4*    m_real;
    volatile LONG       m_ref;

    // ---- async (replacement-buffer) mode ----
    bool                m_async   = false;
    ID3D12Device*       m_device  = nullptr;   // not owned (game's device)
    ID3D12CommandQueue* m_gameQ   = nullptr;   // not owned (game's present queue)
    static constexpr UINT kMaxBuffers = 8;
    ID3D12Resource*     m_buffers[kMaxBuffers] = {};
    UINT                m_bufferCount = 0;
    UINT                m_index   = 0;          // our current back-buffer index
};

// Wrap `real` (any IDXGISwapChain*) in a ProxySwapChain. On success returns the proxy (as
// IDXGISwapChain4*, ref 1) and the caller should release its reference to `real`. Returns nullptr
// if wrapping is disabled (ASYNCREPROJ_NO_PROXY env var) or the real object can't QI to v4 — in
// that case the caller keeps using `real` directly (clean fallback).
IDXGISwapChain4* TryWrapSwapChain(IUnknown* real, ID3D12Device* device, ID3D12CommandQueue* gameQueue);
