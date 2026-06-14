#include "hook_manager.h"
#include "../common/logger.h"
#include "../tracker/resource_tracker.h"
#include "../export/export_manager.h"
#include <detours.h>

HookManager& HookManager::Instance() {
    static HookManager instance;
    return instance;
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

    if (device) {
        void** devVTable = *(void***)device;
        o_CreateCommittedResource = (PFN_CreateCommittedResource)devVTable[27];
        o_CreatePlacedResource = (PFN_CreatePlacedResource)devVTable[29];
        o_CreateShaderResourceView = (PFN_CreateShaderResourceView)devVTable[18];
        o_CreateRenderTargetView = (PFN_CreateRenderTargetView)devVTable[20];
        o_CreateDepthStencilView = (PFN_CreateDepthStencilView)devVTable[21];
    }

    if (queue) {
        void** queueVTable = *(void***)queue;
        o_ExecuteCommandLists = (PFN_ExecuteCommandLists)queueVTable[10];
    }

    if (cmdList) {
        void** listVTable = *(void***)cmdList;
        o_ResourceBarrier = (PFN_ResourceBarrier)listVTable[26];
        o_OMSetRenderTargets = (PFN_OMSetRenderTargets)listVTable[46];
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
    if (o_Present) DetourAttach(&(PVOID&)o_Present, hkPresent);
    if (o_Present1) DetourAttach(&(PVOID&)o_Present1, hkPresent1);
    if (o_CreateCommittedResource) DetourAttach(&(PVOID&)o_CreateCommittedResource, hkCreateCommittedResource);
    if (o_CreatePlacedResource) DetourAttach(&(PVOID&)o_CreatePlacedResource, hkCreatePlacedResource);
    if (o_ExecuteCommandLists) DetourAttach(&(PVOID&)o_ExecuteCommandLists, hkExecuteCommandLists);
    if (o_ResourceBarrier) DetourAttach(&(PVOID&)o_ResourceBarrier, hkResourceBarrier);
    if (o_OMSetRenderTargets) DetourAttach(&(PVOID&)o_OMSetRenderTargets, hkOMSetRenderTargets);
    if (o_CreateShaderResourceView) DetourAttach(&(PVOID&)o_CreateShaderResourceView, hkCreateShaderResourceView);
    if (o_CreateRenderTargetView) DetourAttach(&(PVOID&)o_CreateRenderTargetView, hkCreateRenderTargetView);
    if (o_CreateDepthStencilView) DetourAttach(&(PVOID&)o_CreateDepthStencilView, hkCreateDepthStencilView);

    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        m_hooksInstalled = true;
        LOG_INFO("Detours: All hooks successfully attached.");
        return true;
    } else {
        LOG_ERROR("Detours: Failed to commit hooks! Error code: %ld", err);
        return false;
    }
}

void HookManager::UninstallHooks() {
    if (!m_hooksInstalled) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateSwapChain) DetourDetach(&(PVOID&)o_CreateSwapChain, hkCreateSwapChain);
    if (o_CreateSwapChainForHwnd) DetourDetach(&(PVOID&)o_CreateSwapChainForHwnd, hkCreateSwapChainForHwnd);
    if (o_Present) DetourDetach(&(PVOID&)o_Present, hkPresent);
    if (o_Present1) DetourDetach(&(PVOID&)o_Present1, hkPresent1);
    if (o_CreateCommittedResource) DetourDetach(&(PVOID&)o_CreateCommittedResource, hkCreateCommittedResource);
    if (o_CreatePlacedResource) DetourDetach(&(PVOID&)o_CreatePlacedResource, hkCreatePlacedResource);
    if (o_ExecuteCommandLists) DetourDetach(&(PVOID&)o_ExecuteCommandLists, hkExecuteCommandLists);
    if (o_ResourceBarrier) DetourDetach(&(PVOID&)o_ResourceBarrier, hkResourceBarrier);
    if (o_OMSetRenderTargets) DetourDetach(&(PVOID&)o_OMSetRenderTargets, hkOMSetRenderTargets);
    if (o_CreateShaderResourceView) DetourDetach(&(PVOID&)o_CreateShaderResourceView, hkCreateShaderResourceView);
    if (o_CreateRenderTargetView) DetourDetach(&(PVOID&)o_CreateRenderTargetView, hkCreateRenderTargetView);
    if (o_CreateDepthStencilView) DetourDetach(&(PVOID&)o_CreateDepthStencilView, hkCreateDepthStencilView);

    DetourTransactionCommit();
    m_hooksInstalled = false;
    LOG_INFO("Detours: All hooks successfully detached.");
}

HRESULT WINAPI HookManager::hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LOG_INFO("CreateSwapChain hooked call: Width=%u, Height=%u, Format=%u", pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->BufferDesc.Format);
    
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        ExportManager::Instance().SetActiveCommandQueue(queue);
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
            ExportManager::Instance().SetDevice(device);
            device->Release();
        }
        queue->Release();
    }

    HRESULT hr = HookManager::Instance().o_CreateSwapChain(This, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ExportManager::Instance().InitializeSwapChain(*ppSwapChain);
    }
    return hr;
}

HRESULT WINAPI HookManager::hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LOG_INFO("CreateSwapChainForHwnd hooked call: Width=%u, Height=%u, Format=%u", pDesc->Width, pDesc->Height, pDesc->Format);

    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        ExportManager::Instance().SetActiveCommandQueue(queue);
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
            ExportManager::Instance().SetDevice(device);
            device->Release();
        }
        queue->Release();
    }

    HRESULT hr = HookManager::Instance().o_CreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ExportManager::Instance().InitializeSwapChain(*ppSwapChain);
    }
    return hr;
}

HRESULT WINAPI HookManager::hkPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    ExportManager::Instance().OnPresent(This);
    return HookManager::Instance().o_Present(This, SyncInterval, Flags);
}

HRESULT WINAPI HookManager::hkPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    ExportManager::Instance().OnPresent(This);
    return HookManager::Instance().o_Present1(This, SyncInterval, Flags, pPresentParameters);
}

HRESULT WINAPI HookManager::hkCreateCommittedResource(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidGuid, void** ppvResource) {
    HRESULT hr = HookManager::Instance().o_CreateCommittedResource(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidGuid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = (ID3D12Resource*)*ppvResource;
        ResourceTracker::Instance().RegisterResource(res, pDesc);
    }
    return hr;
}

HRESULT WINAPI HookManager::hkCreatePlacedResource(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidGuid, void** ppvResource) {
    HRESULT hr = HookManager::Instance().o_CreatePlacedResource(This, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riidGuid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = (ID3D12Resource*)*ppvResource;
        ResourceTracker::Instance().RegisterResource(res, pDesc);
    }
    return hr;
}

void WINAPI HookManager::hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (This->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ExportManager::Instance().SetActiveCommandQueue(This);
    }
    HookManager::Instance().o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
}

void WINAPI HookManager::hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    for (UINT i = 0; i < NumBarriers; i++) {
        const auto& barrier = pBarriers[i];
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            ResourceTracker::Instance().OnResourceTransition(
                barrier.Transition.pResource,
                barrier.Transition.StateBefore,
                barrier.Transition.StateAfter
            );
        }
    }
    HookManager::Instance().o_ResourceBarrier(This, NumBarriers, pBarriers);
}

void WINAPI HookManager::hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    ResourceTracker::Instance().OnOMSetRenderTargets(
        This,
        NumRenderTargetDescriptors,
        pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange,
        pDepthStencilDescriptor
    );
    HookManager::Instance().o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

void WINAPI HookManager::hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (pResource) {
        ResourceTracker::Instance().RegisterDescriptor(DestDescriptor, pResource);
    }
    HookManager::Instance().o_CreateShaderResourceView(This, pResource, pDesc, DestDescriptor);
}

void WINAPI HookManager::hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (pResource) {
        ResourceTracker::Instance().RegisterDescriptor(DestDescriptor, pResource);
    }
    HookManager::Instance().o_CreateRenderTargetView(This, pResource, pDesc, DestDescriptor);
}

void WINAPI HookManager::hkCreateDepthStencilView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (pResource) {
        ResourceTracker::Instance().RegisterDescriptor(DestDescriptor, pResource);
    }
    HookManager::Instance().o_CreateDepthStencilView(This, pResource, pDesc, DestDescriptor);
}
