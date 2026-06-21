#include "hook_manager.h"
#include "../common/logger.h"
#include "../overlay/overlay.h"
#include "../swapchain/proxy_swapchain.h"
#include "../present/presenter.h"
#include "../capture/depth_capture.h"
#include <detours.h>

HWND HookManager::s_gameHwnd = nullptr;

HookManager& HookManager::Instance() {
    static HookManager instance;
    return instance;
}

HWND HookManager::GetGameHwnd() {
    return s_gameHwnd;
}

HRESULT HookManager::CallRealPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags) {
    return Instance().o_Present(sc, SyncInterval, Flags);
}

HRESULT HookManager::CallRealPresent1(IDXGISwapChain1* sc, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams) {
    return Instance().o_Present1(sc, SyncInterval, Flags, pParams);
}

bool HookManager::InitializeVTables() {
    if (m_initialized) return true;

    // 1. Create a dummy window
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "DummyWindowForVTables", NULL };
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, "DummyWindowForVTables", "Dummy", WS_OVERLAPPEDWINDOW, 100, 100, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        LOG_ERROR("Failed to create dummy window!");
        return false;
    }

    // 2. Load d3d12.dll
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        LOG_ERROR("Failed to load d3d12.dll!");
        DestroyWindow(hwnd);
        UnregisterClassA("DummyWindowForVTables", wc.hInstance);
        return false;
    }

    auto pfnCreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pfnCreateDevice) {
        LOG_ERROR("Failed to get D3D12CreateDevice proc address!");
        DestroyWindow(hwnd);
        UnregisterClassA("DummyWindowForVTables", wc.hInstance);
        return false;
    }

    ID3D12Device* device = nullptr;
    HRESULT hr = pfnCreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create dummy D3D12 device! hr = 0x%X", hr);
        DestroyWindow(hwnd);
        UnregisterClassA("DummyWindowForVTables", wc.hInstance);
        return false;
    }

    // 3. Create dummy command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create dummy command queue!");
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassA("DummyWindowForVTables", wc.hInstance);
        return false;
    }

    // 4. Create dummy command allocator and command list
    ID3D12CommandAllocator* allocator = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ID3D12GraphicsCommandList* cmdList = nullptr;
    if (allocator) {
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmdList));
    }

    // 5. Create dummy swapchain
    IDXGIFactory4* factory = nullptr;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create DXGI factory!");
        if (cmdList) cmdList->Release();
        if (allocator) allocator->Release();
        queue->Release();
        device->Release();
        DestroyWindow(hwnd);
        UnregisterClassA("DummyWindowForVTables", wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    hr = factory->CreateSwapChain(queue, &sd, &swapChain);
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(hr)) {
        swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1));
    } else {
        LOG_ERROR("Failed to create dummy swapchain! hr = 0x%X", hr);
    }

    // 6. Extract vtables and pointers
    if (factory) {
        void** factoryVTable = *(void***)factory;
        o_CreateSwapChain = (PFN_CreateSwapChain)factoryVTable[10];
        o_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)factoryVTable[15];
    }

    if (swapChain) {
        void** scVTable = *(void***)swapChain;
        o_Present = (PFN_Present)scVTable[8];
    }

    if (swapChain1) {
        void** scVTable1 = *(void***)swapChain1;
        o_Present1 = (PFN_Present1)scVTable1[22];
    }

    // 7. Cleanup dummy objects
    if (swapChain1) swapChain1->Release();
    if (swapChain) swapChain->Release();
    if (factory) factory->Release();
    if (cmdList) cmdList->Release();
    if (allocator) allocator->Release();
    if (queue) queue->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassA("DummyWindowForVTables", wc.hInstance);

    m_initialized = true;
    return true;
}

bool HookManager::InstallHooks() {
    if (m_hooksInstalled) return true;
    if (!InitializeVTables()) return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateSwapChain) DetourAttach(&(PVOID&)o_CreateSwapChain, hkCreateSwapChain);
    if (o_CreateSwapChainForHwnd) DetourAttach(&(PVOID&)o_CreateSwapChainForHwnd, hkCreateSwapChainForHwnd);

    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        m_hooksInstalled = true;
        LOG_INFO("Detours: swapchain-create hooks attached (lean low-latency build).");
        DepthCapture::Install();   // late-hook the FSR3.1 ffx-api for depth/MV interception
        return true;
    }
    LOG_ERROR("Detours: failed to commit hooks! Error code: %ld", err);
    return false;
}

void HookManager::UninstallHooks() {
    if (!m_hooksInstalled) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_CreateSwapChain) DetourDetach(&(PVOID&)o_CreateSwapChain, hkCreateSwapChain);
    if (o_CreateSwapChainForHwnd) DetourDetach(&(PVOID&)o_CreateSwapChainForHwnd, hkCreateSwapChainForHwnd);
    DetourTransactionCommit();
    DepthCapture::Uninstall();
    m_hooksInstalled = false;

    // Stop the presenter thread before the process tears down its D3D objects.
    Presenter::Shutdown();
    LOG_INFO("Detours: hooks detached.");
}

HRESULT WINAPI HookManager::hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LOG_INFO("CreateSwapChain hooked: %ux%u fmt=%u", pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->BufferDesc.Format);
    if (pDesc && pDesc->OutputWindow) s_gameHwnd = pDesc->OutputWindow;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        Overlay::SetPresentQueue(queue);
        queue->GetDevice(IID_PPV_ARGS(&device));
    }

    // In async mode the real swapchain must present on the presenter's queue, not the game's.
    IUnknown* createDevice = pDevice;
    if (Presenter::AsyncEnabled() && device) {
        if (ID3D12CommandQueue* pq = Presenter::EnsureQueue(device)) createDevice = pq;
    }

    // Note: flip-model swapchains forbid DXGI_USAGE_UNORDERED_ACCESS, so the compute warp can't write
    // the backbuffer directly — it warps into a scratch UAV and CopyResource's into the backbuffer.
    HRESULT hr = HookManager::Instance().o_CreateSwapChain(This, createDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        if (IDXGISwapChain4* proxy = TryWrapSwapChain(*ppSwapChain, device, queue)) {
            (*ppSwapChain)->Release();
            *ppSwapChain = proxy;
        }
    }
    if (device) device->Release();
    if (queue)  queue->Release();
    return hr;
}

HRESULT WINAPI HookManager::hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LOG_INFO("CreateSwapChainForHwnd hooked: %ux%u fmt=%u", pDesc->Width, pDesc->Height, pDesc->Format);
    if (hWnd) s_gameHwnd = hWnd;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        Overlay::SetPresentQueue(queue);
        queue->GetDevice(IID_PPV_ARGS(&device));
    }

    IUnknown* createDevice = pDevice;
    if (Presenter::AsyncEnabled() && device) {
        if (ID3D12CommandQueue* pq = Presenter::EnsureQueue(device)) createDevice = pq;
    }

    // Note: flip-model swapchains forbid DXGI_USAGE_UNORDERED_ACCESS, so the compute warp can't write
    // the backbuffer directly — it warps into a scratch UAV and CopyResource's into the backbuffer.
    HRESULT hr = HookManager::Instance().o_CreateSwapChainForHwnd(This, createDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        if (IDXGISwapChain4* proxy = TryWrapSwapChain(*ppSwapChain, device, queue)) {
            (*ppSwapChain)->Release();
            *ppSwapChain = proxy;
        }
    }
    if (device) device->Release();
    if (queue)  queue->Release();
    return hr;
}
