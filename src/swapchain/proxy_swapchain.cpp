#include "proxy_swapchain.h"
#include "../common/logger.h"
#include "../hooks/hook_manager.h"
#include "../overlay/overlay.h"
#include "../warp/warp_renderer.h"
#include "../present/presenter.h"

// Shared pre-present work for the SYNCHRONOUS path: warp the backbuffer (if enabled), then draw the
// overlay. (Async mode does none of this here — the presenter thread owns it.)
static void RunPrePresent(IDXGISwapChain4* sc) {
    if (ID3D12CommandQueue* q = Overlay::GetPresentQueue()) {
        UINT idx = sc->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb))) && bb) {
            WarpRenderer::Instance().Render(q, bb);
            bb->Release();
        }
    }
    Overlay::RenderOverlay(sc);
}

ProxySwapChain::ProxySwapChain(IDXGISwapChain4* real, ID3D12Device* device, ID3D12CommandQueue* gameQueue)
    : m_real(real), m_ref(1), m_async(true), m_device(device), m_gameQ(gameQueue) {
    AllocReplacementBuffers();
    // The real swapchain was created on the presenter's queue; start the presenter against it.
    Presenter::Start(m_real, m_gameQ, m_device);
}

ProxySwapChain::~ProxySwapChain() {
    if (m_async) {
        Presenter::Stop();
        ReleaseReplacementBuffers();
    }
}

void ProxySwapChain::AllocReplacementBuffers() {
    DXGI_SWAP_CHAIN_DESC1 d = {};
    if (FAILED(m_real->GetDesc1(&d))) { LOG_ERROR("Proxy(async): GetDesc1 failed"); return; }
    m_bufferCount = d.BufferCount;
    if (m_bufferCount > kMaxBuffers) m_bufferCount = kMaxBuffers;
    m_index = 0;

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = d.Width; rd.Height = d.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = d.Format; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // game renders into these; we also SRV them

    for (UINT i = 0; i < m_bufferCount; ++i) {
        // COMMON == PRESENT (0): the game treats these as swapchain backbuffers and transitions
        // PRESENT<->RENDER_TARGET around its rendering, exactly as it would for real ones.
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_buffers[i])))) {
            LOG_ERROR("Proxy(async): failed to create replacement buffer %u (%ux%u fmt=%u)",
                      i, d.Width, d.Height, (unsigned)d.Format);
            m_buffers[i] = nullptr;
        }
    }
    LOG_INFO("Proxy(async): allocated %u replacement buffers %ux%u fmt=%u",
             m_bufferCount, d.Width, d.Height, (unsigned)d.Format);
}

void ProxySwapChain::ReleaseReplacementBuffers() {
    for (UINT i = 0; i < kMaxBuffers; ++i) {
        if (m_buffers[i]) { m_buffers[i]->Release(); m_buffers[i] = nullptr; }
    }
    m_bufferCount = 0;
    m_index = 0;
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) ||
        riid == __uuidof(IDXGISwapChain1) ||
        riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) ||
        riid == __uuidof(IDXGISwapChain4)) {
        AddRef();
        *ppv = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    // Anything else (debug interfaces, media, etc.) — hand back the real object directly.
    return m_real->QueryInterface(riid, ppv);
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    if (m_async) {
        // Hand the game our replacement buffer; it renders into this thinking it's a real backbuffer.
        if (Buffer < m_bufferCount && m_buffers[Buffer])
            return m_buffers[Buffer]->QueryInterface(riid, ppSurface);
        return E_INVALIDARG;
    }
    return m_real->GetBuffer(Buffer, riid, ppSurface);
}

UINT STDMETHODCALLTYPE ProxySwapChain::GetCurrentBackBufferIndex() {
    if (m_async) return m_index;
    return m_real->GetCurrentBackBufferIndex();
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::Present(UINT SyncInterval, UINT Flags) {
    if (m_async) {
        // FG is off, so every Present is a real game frame: hand the just-finished replacement buffer
        // to the presenter and return — the game keeps its own cadence, the presenter warps + presents.
        Presenter::SubmitGameFrame(m_buffers[m_index]);
        m_index = (m_index + 1) % m_bufferCount;
        return S_OK;
    }
    RunPrePresent(m_real);
    // Forward via the un-hooked trampoline so the vtable Present hook doesn't also fire (no double).
    return HookManager::CallRealPresent(m_real, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if (m_async) {
        Presenter::SubmitGameFrame(m_buffers[m_index]);
        m_index = (m_index + 1) % m_bufferCount;
        return S_OK;
    }
    RunPrePresent(m_real);
    return HookManager::CallRealPresent1(m_real, SyncInterval, PresentFlags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE ProxySwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    if (m_async) {
        // Drain + tear down the presenter and our buffers, resize the real swapchain, rebuild, restart.
        Presenter::Stop();
        ReleaseReplacementBuffers();
        HRESULT hr = m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
        if (FAILED(hr)) { LOG_ERROR("Proxy(async): real ResizeBuffers failed 0x%X", hr); return hr; }
        AllocReplacementBuffers();
        Presenter::Start(m_real, m_gameQ, m_device);
        return hr;
    }
    return m_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

IDXGISwapChain4* TryWrapSwapChain(IUnknown* real, ID3D12Device* device, ID3D12CommandQueue* gameQueue) {
    if (!real) return nullptr;

    if (GetEnvironmentVariableW(L"ASYNCREPROJ_NO_PROXY", nullptr, 0) != 0) {
        LOG_INFO("ProxySwapChain: disabled via ASYNCREPROJ_NO_PROXY — using real swapchain");
        return nullptr;
    }

    IDXGISwapChain4* real4 = nullptr;
    if (FAILED(real->QueryInterface(IID_PPV_ARGS(&real4))) || !real4) {
        LOG_ERROR("ProxySwapChain: real swapchain does not support IDXGISwapChain4 — falling back");
        return nullptr;
    }

    // real4 holds one reference; transfer it into the proxy.
    if (Presenter::AsyncEnabled() && device && gameQueue) {
        ProxySwapChain* proxy = new ProxySwapChain(real4, device, gameQueue);
        LOG_INFO("ProxySwapChain: wrapped (ASYNC) real 0x%p (proxy 0x%p)", real4, proxy);
        return proxy;
    }
    ProxySwapChain* proxy = new ProxySwapChain(real4);
    LOG_INFO("ProxySwapChain: wrapped (sync) real 0x%p (proxy 0x%p)", real4, proxy);
    return proxy;
}
