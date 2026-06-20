#include "hook_manager.h"
#include "../common/logger.h"
#include "rt_tracker.h"
#include "../tracker/resource_tracker.h"
#include "../export/export_manager.h"
#include "../overlay/overlay.h"
#include "../swapchain/proxy_swapchain.h"
#include "../present/presenter.h"
#include <detours.h>
#include <string>
#include <algorithm>
#include <cwctype>

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

    o_LoadLibraryW = ::LoadLibraryW;
    o_LoadLibraryExW = ::LoadLibraryExW;
    o_LoadLibraryA = ::LoadLibraryA;
    o_LoadLibraryExA = ::LoadLibraryExA;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateSwapChain) DetourAttach(&(PVOID&)o_CreateSwapChain, hkCreateSwapChain);
    if (o_CreateSwapChainForHwnd) DetourAttach(&(PVOID&)o_CreateSwapChainForHwnd, hkCreateSwapChainForHwnd);
    if (o_Present) DetourAttach(&(PVOID&)o_Present, hkPresent);
    if (o_Present1) DetourAttach(&(PVOID&)o_Present1, hkPresent1);

    if (o_LoadLibraryW) DetourAttach(&(PVOID&)o_LoadLibraryW, hkLoadLibraryW);
    if (o_LoadLibraryExW) DetourAttach(&(PVOID&)o_LoadLibraryExW, hkLoadLibraryExW);
    if (o_LoadLibraryA) DetourAttach(&(PVOID&)o_LoadLibraryA, hkLoadLibraryA);
    if (o_LoadLibraryExA) DetourAttach(&(PVOID&)o_LoadLibraryExA, hkLoadLibraryExA);

    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        m_hooksInstalled = true;
        LOG_INFO("Detours: All hooks successfully attached.");

        // Check if upscalers are already loaded
        HMODULE hNVNGX = GetModuleHandleW(L"nvngx.dll");
        if (hNVNGX) HookNVNGX(hNVNGX);

        HMODULE hFSR2 = GetModuleHandleW(L"ffx_fsr2_api_dx12_x64.dll");
        if (!hFSR2) hFSR2 = GetModuleHandleW(L"ffx_fsr2_api_dx12.dll");
        if (hFSR2) HookFSR2(hFSR2);

        HMODULE hFSR3_1 = GetModuleHandleW(L"ffx_fsr3_dx12_x64.dll");
        if (hFSR3_1) HookFSR3(hFSR3_1);
        HMODULE hFSR3_2 = GetModuleHandleW(L"ffx_fsr3_dx12.dll");
        if (hFSR3_2) HookFSR3(hFSR3_2);
        HMODULE hFSR3_3 = GetModuleHandleW(L"ffx_fsr3_x64.dll");
        if (hFSR3_3) HookFSR3(hFSR3_3);
        HMODULE hFSR3_4 = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
        if (hFSR3_4) HookFSR3(hFSR3_4);

        // FG backend (separate DLLs from the upscaler) — for FG-F0 swapchain coexistence.
        HMODULE hFG1 = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
        if (hFG1) HookFGBackend(hFG1);
        HMODULE hFG2 = GetModuleHandleW(L"ffx_backend_dx12_x64.dll");
        if (hFG2) HookFGBackend(hFG2);
        HMODULE hFG3 = GetModuleHandleW(L"amd_fidelityfx_dx12.dll");
        if (hFG3) HookFGBackend(hFG3);

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

    if (o_LoadLibraryW) DetourDetach(&(PVOID&)o_LoadLibraryW, hkLoadLibraryW);
    if (o_LoadLibraryExW) DetourDetach(&(PVOID&)o_LoadLibraryExW, hkLoadLibraryExW);
    if (o_LoadLibraryA) DetourDetach(&(PVOID&)o_LoadLibraryA, hkLoadLibraryA);
    if (o_LoadLibraryExA) DetourDetach(&(PVOID&)o_LoadLibraryExA, hkLoadLibraryExA);

    if (o_NVSDK_NGX_D3D12_EvaluateFeature) DetourDetach(&(PVOID&)o_NVSDK_NGX_D3D12_EvaluateFeature, hkNVSDK_NGX_D3D12_EvaluateFeature);
    if (o_ffxFsr2ContextEvaluate) DetourDetach(&(PVOID&)o_ffxFsr2ContextEvaluate, hkffxFsr2ContextEvaluate);
    if (o_ffxFsr3ContextEvaluate) DetourDetach(&(PVOID&)o_ffxFsr3ContextEvaluate, hkffxFsr3ContextEvaluate);
    if (o_ffxFsr3ContextDispatchUpscale) DetourDetach(&(PVOID&)o_ffxFsr3ContextDispatchUpscale, hkffxFsr3ContextDispatchUpscale);
    if (o_ffxFsr3UpscalerContextDispatch) DetourDetach(&(PVOID&)o_ffxFsr3UpscalerContextDispatch, hkffxFsr3UpscalerContextDispatch);
    if (o_ffxFsr3ConfigureFrameGeneration) DetourDetach(&(PVOID&)o_ffxFsr3ConfigureFrameGeneration, hkffxFsr3ConfigureFrameGeneration);
    if (o_ffxSetFrameGenerationConfigToSwapchainDX12) DetourDetach(&(PVOID&)o_ffxSetFrameGenerationConfigToSwapchainDX12, hkffxSetFrameGenerationConfigToSwapchainDX12);
    if (o_ffxCreateFISwapchainForHwndDX12) DetourDetach(&(PVOID&)o_ffxCreateFISwapchainForHwndDX12, hkffxCreateFrameinterpolationSwapchainForHwndDX12);
    if (o_ffxReplaceSwapchainForFIDX12) DetourDetach(&(PVOID&)o_ffxReplaceSwapchainForFIDX12, hkffxReplaceSwapchainForFrameinterpolationDX12);
    if (o_ffxFsr3DispatchFrameGeneration) DetourDetach(&(PVOID&)o_ffxFsr3DispatchFrameGeneration, hkffxFsr3DispatchFrameGeneration);
    if (o_ffxRegisterFrameinterpolationUiResourceDX12) DetourDetach(&(PVOID&)o_ffxRegisterFrameinterpolationUiResourceDX12, hkffxRegisterFrameinterpolationUiResourceDX12);
    if (o_ffxConfigure) DetourDetach(&(PVOID&)o_ffxConfigure, hkffxConfigure);
    if (o_ffxDispatch) DetourDetach(&(PVOID&)o_ffxDispatch, hkffxDispatch);

    DetourTransactionCommit();
    m_hooksInstalled = false;

    // Stop the presenter thread (if it ran) before the process tears down its D3D objects.
    Presenter::Shutdown();
    LOG_INFO("Detours: All hooks successfully detached.");
}

HRESULT WINAPI HookManager::hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LOG_INFO("CreateSwapChain hooked call: Width=%u, Height=%u, Format=%u", pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->BufferDesc.Format);
    // FG-F0: if this call is nested inside FFX's FG-swapchain creation, pass the real swapchain
    // through un-wrapped so FFX owns it (no double-wrap -> no device removal).
    if (HookManager::Instance().m_insideFgSwapchainCreate) {
        LOG_INFO("  (FG-create in progress: passing real swapchain through, no async proxy)");
        return HookManager::Instance().o_CreateSwapChain(This, pDevice, pDesc, ppSwapChain);
    }
    if (pDesc && pDesc->OutputWindow) s_gameHwnd = pDesc->OutputWindow;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        ExportManager::Instance().SetActiveCommandQueue(queue);
        Overlay::SetPresentQueue(queue);
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
            ExportManager::Instance().SetDevice(device);
        }
    }

    // In async mode the real swapchain must present on the presenter's queue, not the game's, so the
    // game-supplied queue is swapped for the presenter queue in the real CreateSwapChain call.
    IUnknown* createDevice = pDevice;
    if (Presenter::AsyncEnabled() && device) {
        if (ID3D12CommandQueue* pq = Presenter::EnsureQueue(device)) createDevice = pq;
    }

    HRESULT hr = HookManager::Instance().o_CreateSwapChain(This, createDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ExportManager::Instance().InitializeSwapChain(*ppSwapChain);
        if (IDXGISwapChain4* proxy = TryWrapSwapChain(*ppSwapChain, device, queue)) {
            (*ppSwapChain)->Release();   // drop the caller's original ref — the proxy now owns the real
            *ppSwapChain = proxy;        // hand the game the proxy
        }
    }
    if (device) device->Release();
    if (queue)  queue->Release();
    return hr;
}

HRESULT WINAPI HookManager::hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LOG_INFO("CreateSwapChainForHwnd hooked call: Width=%u, Height=%u, Format=%u", pDesc->Width, pDesc->Height, pDesc->Format);
    // FG-F0: stand down while FFX is building its FG swapchain — let the real one through un-wrapped.
    if (HookManager::Instance().m_insideFgSwapchainCreate) {
        LOG_INFO("  (FG-create in progress: passing real swapchain through, no async proxy)");
        return HookManager::Instance().o_CreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }
    if (hWnd) s_gameHwnd = hWnd;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
        ExportManager::Instance().SetActiveCommandQueue(queue);
        Overlay::SetPresentQueue(queue);
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
            ExportManager::Instance().SetDevice(device);
        }
    }

    IUnknown* createDevice = pDevice;
    if (Presenter::AsyncEnabled() && device) {
        if (ID3D12CommandQueue* pq = Presenter::EnsureQueue(device)) createDevice = pq;
    }

    HRESULT hr = HookManager::Instance().o_CreateSwapChainForHwnd(This, createDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        ExportManager::Instance().InitializeSwapChain(*ppSwapChain);
        if (IDXGISwapChain4* proxy = TryWrapSwapChain(*ppSwapChain, device, queue)) {
            (*ppSwapChain)->Release();   // drop the caller's original ref — the proxy now owns the real
            *ppSwapChain = proxy;        // hand the game the proxy
        }
    }
    if (device) device->Release();
    if (queue)  queue->Release();
    return hr;
}

HRESULT WINAPI HookManager::hkPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
    ExportManager::Instance().OnPresent(This);
    Overlay::RenderOverlay(This);
    return HookManager::Instance().o_Present(This, SyncInterval, Flags);
}

HRESULT WINAPI HookManager::hkPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    ExportManager::Instance().OnPresent(This);
    Overlay::RenderOverlay(This);
    return HookManager::Instance().o_Present1(This, SyncInterval, Flags, pPresentParameters);
}

HMODULE WINAPI HookManager::hkLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = HookManager::Instance().o_LoadLibraryW(lpLibFileName);
    if (hMod && lpLibFileName) {
        HookManager::Instance().CheckAndHookUpscaler(lpLibFileName, hMod);
    }
    return hMod;
}

HMODULE WINAPI HookManager::hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = HookManager::Instance().o_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hMod && lpLibFileName) {
        HookManager::Instance().CheckAndHookUpscaler(lpLibFileName, hMod);
    }
    return hMod;
}

HMODULE WINAPI HookManager::hkLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hMod = HookManager::Instance().o_LoadLibraryA(lpLibFileName);
    if (hMod && lpLibFileName) {
        wchar_t wstr[512] = {0};
        if (MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, wstr, 512) > 0) {
            HookManager::Instance().CheckAndHookUpscaler(wstr, hMod);
        }
    }
    return hMod;
}

HMODULE WINAPI HookManager::hkLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = HookManager::Instance().o_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (hMod && lpLibFileName) {
        wchar_t wstr[512] = {0};
        if (MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, wstr, 512) > 0) {
            HookManager::Instance().CheckAndHookUpscaler(wstr, hMod);
        }
    }
    return hMod;
}

int WINAPI HookManager::hkNVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList, const void* InFeatureHandle, const NVSDK_NGX_Parameter* InParameters, void* InCallback) {
    if (InParameters) {
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* mv = nullptr;
        const_cast<NVSDK_NGX_Parameter*>(InParameters)->Get("Depth", (void**)&depth);
        const_cast<NVSDK_NGX_Parameter*>(InParameters)->Get("MotionVectors", (void**)&mv);

        if (depth || mv) {
            {
                std::lock_guard<std::mutex> lock(HookManager::Instance().m_upscalerMutex);
                if (depth) HookManager::Instance().m_interceptedDepth = depth;
                if (mv) HookManager::Instance().m_interceptedMV = mv;
            }
            // Install destruction callbacks so these pointers get nulled if the engine
            // frees the resources (idempotent — only the first call per resource installs).
            ResourceTracker::Instance().TrackForDestruction(depth);
            ResourceTracker::Instance().TrackForDestruction(mv);
        }
    }
    return HookManager::Instance().o_NVSDK_NGX_D3D12_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
}

int WINAPI HookManager::hkffxFsr2ContextEvaluate(void* context, const FfxFsr2DispatchDescription* dispatchDescription) {
    if (dispatchDescription) {
        ID3D12Resource* depth = (ID3D12Resource*)dispatchDescription->depth.resource;
        ID3D12Resource* mv = (ID3D12Resource*)dispatchDescription->motionVectors.resource;

        {
            std::lock_guard<std::mutex> lock(HookManager::Instance().m_upscalerMutex);
            if (depth) HookManager::Instance().m_interceptedDepth = depth;
            if (mv) HookManager::Instance().m_interceptedMV = mv;
        }
        // The FSR2 dispatch contract hands us the real depth/MV resources directly, same as
        // the FSR3 path, so accept them without consulting a tracked-resource set. Install
        // destruction callbacks so the pointers get nulled if the engine frees them.
        ResourceTracker::Instance().TrackForDestruction(depth);
        ResourceTracker::Instance().TrackForDestruction(mv);

        // v3: capture reprojection params from the full FSR2 dispatch description.
        ExportManager::Instance().SetUpscalerParams(
            dispatchDescription->motionVectorScale.x, dispatchDescription->motionVectorScale.y,
            dispatchDescription->jitterOffset.x,      dispatchDescription->jitterOffset.y,
            dispatchDescription->renderSize.width,    dispatchDescription->renderSize.height,
            dispatchDescription->cameraNear,          dispatchDescription->cameraFar,
            dispatchDescription->cameraFovAngleVertical);
    }
    return HookManager::Instance().o_ffxFsr2ContextEvaluate(context, dispatchDescription);
}

static void LogResourceDetails(const char* name, ID3D12Resource* res) {
    if (!res) {
        LOG_INFO("  %s: NULL", name);
        return;
    }
    if (((uintptr_t)res & 7) != 0) {
        LOG_INFO("  %s: UNALIGNED POINTER 0x%p", name, res);
        return;
    }
    __try {
        D3D12_RESOURCE_DESC desc = res->GetDesc();
        bool isTypeless = (desc.Format == DXGI_FORMAT_R32_TYPELESS ||
                           desc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
                           desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                           desc.Format == DXGI_FORMAT_R16_TYPELESS ||
                           desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
        bool isDSV = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        bool isUAV = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        bool isSRV = !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);

        LOG_INFO("  %s: ptr=0x%p | Size=%ux%u | Format=%u | Flags=0x%X | Mips=%u | Samples=%u | ArraySize=%u | Layout=%d | Dim=%d | Typeless=%d | DSV=%d | UAV=%d | SRV=%d",
                 name, res, (uint32_t)desc.Width, (uint32_t)desc.Height, desc.Format, 
                 (uint32_t)desc.Flags, desc.MipLevels, desc.SampleDesc.Count, desc.DepthOrArraySize, (int)desc.Layout, (int)desc.Dimension, isTypeless, isDSV, isUAV, isSRV);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("  %s: CRASH AVOIDED while reading resource 0x%p!", name, res);
    }
}

static void ExtractInterceptedResources(const FfxFsr3DispatchDescription* dispatchDescription, ID3D12Resource** outColor, ID3D12Resource** outDepth, ID3D12Resource** outMV) {
    *outColor = nullptr;
    *outDepth = nullptr;
    *outMV = nullptr;
    if (!dispatchDescription) return;

    ID3D12Resource* detectedColor = (ID3D12Resource*)dispatchDescription->color.resource;
    ID3D12Resource* detectedDepth = (ID3D12Resource*)dispatchDescription->depth.resource;
    ID3D12Resource* detectedMV = (ID3D12Resource*)dispatchDescription->motionVectors.resource;

    // Log properties when they change to prevent flooding the log
    static ID3D12Resource* lastColor = nullptr;
    static ID3D12Resource* lastDepth = nullptr;
    static ID3D12Resource* lastMV = nullptr;

    if (detectedColor != lastColor || detectedDepth != lastDepth || detectedMV != lastMV) {
        lastColor = detectedColor;
        lastDepth = detectedDepth;
        lastMV = detectedMV;
        LOG_INFO("Upscaler Intercepted Resources Changed:");
        LogResourceDetails("Color", detectedColor);
        LogResourceDetails("Depth", detectedDepth);
        LogResourceDetails("Motion Vectors", detectedMV);
    }

    *outColor = detectedColor;
    *outDepth = detectedDepth;
    *outMV = detectedMV;
}

struct Fsr3DispatchDecoded {
    float motionVectorScaleX;
    float motionVectorScaleY;
    float jitterOffsetX;
    float jitterOffsetY;
    unsigned int renderWidth;
    unsigned int renderHeight;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
};

static Fsr3DispatchDecoded DecodeFsr3Dispatch(const void* descPtr) {
    Fsr3DispatchDecoded decoded = {};
    if (!descPtr) return decoded;

    const char* base = (const char*)descPtr;

    // Check size at 3.1 offset (1784) vs 3.0 offset (1256)
    const FfxDimensions2D* size31 = (const FfxDimensions2D*)(base + 1784);
    const FfxDimensions2D* size30 = (const FfxDimensions2D*)(base + 1256);
    
    bool is31 = (size31->width > 100 && size31->width < 10000 &&
                 size31->height > 100 && size31->height < 10000);

    if (is31) {
        // FSR 3.1 layout
        decoded.motionVectorScaleX = *(const float*)(base + 1776);
        decoded.motionVectorScaleY = *(const float*)(base + 1780);
        decoded.jitterOffsetX = *(const float*)(base + 1768);
        decoded.jitterOffsetY = *(const float*)(base + 1772);
        decoded.renderWidth = size31->width;
        decoded.renderHeight = size31->height;
        decoded.cameraNear = *(const float*)(base + 1812);
        decoded.cameraFar = *(const float*)(base + 1816);
        decoded.cameraFovAngleVertical = *(const float*)(base + 1820);
    } else {
        // FSR 3.0 layout
        decoded.motionVectorScaleX = *(const float*)(base + 1248);
        decoded.motionVectorScaleY = *(const float*)(base + 1252);
        decoded.jitterOffsetX = *(const float*)(base + 1240);
        decoded.jitterOffsetY = *(const float*)(base + 1244);
        decoded.renderWidth = size30->width;
        decoded.renderHeight = size30->height;
        decoded.cameraNear = *(const float*)(base + 1284);
        decoded.cameraFar = *(const float*)(base + 1288);
        decoded.cameraFovAngleVertical = *(const float*)(base + 1292);
    }
    return decoded;
}

// Stores intercepted pointers, then records the depth/MV copy onto the game's own command
// list while those resources are still in their FSR-input (shader-read) state — the only
// point where the copy is correctly ordered and the resource state is deterministic.
void HookManager::HandleFsr3Dispatch(const FfxFsr3DispatchDescription* dispatchDescription) {
    ID3D12Resource* color = nullptr, *depth = nullptr, *mv = nullptr;
    ExtractInterceptedResources(dispatchDescription, &color, &depth, &mv);
    {
        static int lastDepthFfx = -1, lastMvFfx = -1;
        int depthFfxState = dispatchDescription->depth.state;
        int mvFfxState    = dispatchDescription->motionVectors.state;
        if (depthFfxState != lastDepthFfx || mvFfxState != lastMvFfx) {
            lastDepthFfx = depthFfxState; lastMvFfx = mvFfxState;
            LOG_INFO("FSR dispatch input states: depth FFX=%d  mv FFX=%d", depthFfxState, mvFfxState);
        }
    }
    {
        std::lock_guard<std::mutex> lock(HookManager::Instance().m_upscalerMutex);
        if (color) HookManager::Instance().m_interceptedColor = color;
        if (depth) HookManager::Instance().m_interceptedDepth = depth;
        if (mv)    HookManager::Instance().m_interceptedMV = mv;
        HookManager::Instance().m_dispatchOccurredThisFrame = true;
    }
    // Install destruction callbacks so the intercepted pointers get nulled if the engine
    // frees the resources (idempotent — only the first call per resource installs).
    ResourceTracker::Instance().TrackForDestruction(color);
    ResourceTracker::Instance().TrackForDestruction(depth);
    ResourceTracker::Instance().TrackForDestruction(mv);

    // v3: capture the reprojection params from the full dispatch description (truncated struct
    // was extended in upscaler_defs.h). Surfaced to the warp via ExportManager::SetUpscalerParams.
    Fsr3DispatchDecoded decoded = DecodeFsr3Dispatch(dispatchDescription);
    ExportManager::Instance().SetUpscalerParams(
        decoded.motionVectorScaleX, decoded.motionVectorScaleY,
        decoded.jitterOffsetX,      decoded.jitterOffsetY,
        decoded.renderWidth,        decoded.renderHeight,
        decoded.cameraNear,          decoded.cameraFar,
        decoded.cameraFovAngleVertical);

    auto* gameList = (ID3D12GraphicsCommandList*)dispatchDescription->commandList;
    ExportManager::Instance().CopyDepthMVOnDispatch(gameList, depth, mv);

    // Hud-less tracker (Track A): install the RT vtable hooks once, here, where we hold both the
    // game's command list (shared vtable) and its device. No-op unless ASYNCREPROJ_HUDLESS=1.
    if (RtTracker::Enabled() && gameList) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(gameList->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            RtTracker::EnsureHooks(dev, gameList);
            dev->Release();
        }
        RtTracker::OnUpscaleDispatch();   // arm the first-after-upscale hud-less capture
    }
}

int WINAPI HookManager::hkffxFsr3ContextEvaluate(void* context, const FfxFsr3DispatchDescription* dispatchDescription) {
    if (dispatchDescription) HandleFsr3Dispatch(dispatchDescription);
    return HookManager::Instance().o_ffxFsr3ContextEvaluate(context, dispatchDescription);
}

int WINAPI HookManager::hkffxFsr3ContextDispatchUpscale(void* context, const FfxFsr3DispatchDescription* dispatchDescription) {
    if (dispatchDescription) HandleFsr3Dispatch(dispatchDescription);
    return HookManager::Instance().o_ffxFsr3ContextDispatchUpscale(context, dispatchDescription);
}

int WINAPI HookManager::hkffxFsr3UpscalerContextDispatch(void* context, const FfxFsr3DispatchDescription* dispatchDescription) {
    if (dispatchDescription) HandleFsr3Dispatch(dispatchDescription);
    return HookManager::Instance().o_ffxFsr3UpscalerContextDispatch(context, dispatchDescription);
}

// Step 1 of the hud-less pivot: log how the game hands over the hud-less / UI when it (re)configures
// Frame Generation. READ-ONLY and crash-safe — we only read inline struct fields (the FfxResource's
// own description/state) and null-check the resource pointer; we never call ->GetDesc() on it (that
// vtable deref is what crashed the upscaler-output probe). Fires only when FG is actually toggled, so
// with FG off this never runs and nothing changes.
// Shared crash-safe logger for an FG config (inline fields only — never derefs the resource).
struct FgConfigDecoded {
    bool frameGenerationEnabled;
    void* presentCallback;
    void* frameGenerationCallback;
    bool onlyPresentInterpolated;
    unsigned int flags;
    void* hudLessResource;
    unsigned int hudLessWidth;
    unsigned int hudLessHeight;
    unsigned int hudLessFormat;
    int hudLessState;
    unsigned int hudLessFlags;
    unsigned int hudLessUsage;
};

static FgConfigDecoded DecodeFgConfig(const void* configPtr) {
    FgConfigDecoded decoded = {};
    if (!configPtr) return decoded;

    // Check if it's the SDK layout (starts with ffxApiHeader)
    uint64_t typeVal = *(const uint64_t*)configPtr;
    bool isSdkLayout = ((typeVal & 0xFFFF0000) == 0x00020000);

    const char* base = (const char*)configPtr;
    if (isSdkLayout) {
        // FSR 3.1 SDK v2 layout
        decoded.frameGenerationEnabled = *(const bool*)(base + 56);
        decoded.presentCallback = *(void**)(base + 24);
        decoded.frameGenerationCallback = *(void**)(base + 40);
        decoded.onlyPresentInterpolated = *(const bool*)(base + 116);
        decoded.flags = *(const uint32_t*)(base + 112);
        
        decoded.hudLessResource = *(void**)(base + 64);
        decoded.hudLessWidth = *(const uint32_t*)(base + 80);
        decoded.hudLessHeight = *(const uint32_t*)(base + 84);
        decoded.hudLessFormat = *(const uint32_t*)(base + 76);
        decoded.hudLessState = *(const int*)(base + 104);
        decoded.hudLessFlags = *(const uint32_t*)(base + 96);
        decoded.hudLessUsage = *(const uint32_t*)(base + 100);
    } else {
        // FSR 3.0 layout
        decoded.frameGenerationEnabled = *(const bool*)(base + 24);
        decoded.presentCallback = *(void**)(base + 8);
        decoded.frameGenerationCallback = *(void**)(base + 16);
        decoded.onlyPresentInterpolated = *(const bool*)(base + 212);
        decoded.flags = *(const uint32_t*)(base + 208);
        
        decoded.hudLessResource = *(void**)(base + 32);
        decoded.hudLessWidth = *(const uint32_t*)(base + 48);
        decoded.hudLessHeight = *(const uint32_t*)(base + 52);
        decoded.hudLessFormat = *(const uint32_t*)(base + 44);
        decoded.hudLessState = *(const int*)(base + 72);
        decoded.hudLessFlags = *(const uint32_t*)(base + 64);
        decoded.hudLessUsage = *(const uint32_t*)(base + 68);
    }
    return decoded;
}

// Shared crash-safe logger for an FG config (inline fields only — never derefs the resource).
static void LogFgConfig(const char* who, const void* configPtr) {
    if (!configPtr) return;
    FgConfigDecoded config = DecodeFgConfig(configPtr);
    LOG_INFO("%s: enabled=%d  presentCallback=%s  frameGenCallback=%s  onlyInterpolated=%d  flags=0x%X",
             who, (int)config.frameGenerationEnabled,
             config.presentCallback ? "YES" : "no",
             config.frameGenerationCallback ? "YES" : "no",
             (int)config.onlyPresentInterpolated, config.flags);
    if (config.hudLessResource) {
        LOG_INFO("  HUDLessColor: ptr=0x%p | Size=%ux%u | ffxFormat=%u | ffxState=%d | flags=0x%X | usage=0x%X",
                 config.hudLessResource, config.hudLessWidth, config.hudLessHeight,
                 config.hudLessFormat, config.hudLessState,
                 config.hudLessFlags, config.hudLessUsage);
    } else {
        LOG_INFO("  HUDLessColor: NULL (game composites UI via presentCallback instead)");
    }
}

int WINAPI HookManager::hkffxFsr3ConfigureFrameGeneration(void* context, const FfxFrameGenerationConfigFSR3* config) {
    static int callCount = 0;
    if (config && ++callCount <= 4) LogFgConfig("FG ConfigureFrameGeneration (high-level)", config);
    if (config) {
        FgConfigDecoded decoded = DecodeFgConfig(config);
        if (decoded.hudLessResource) {
            auto& self = HookManager::Instance();
            {
                std::lock_guard<std::mutex> lock(self.m_upscalerMutex);
                self.m_interceptedFgHudless = (ID3D12Resource*)decoded.hudLessResource;
                self.m_interceptedFgHudlessState = decoded.hudLessState;
            }
            ResourceTracker::Instance().TrackForDestruction((ID3D12Resource*)decoded.hudLessResource);
        }
    }
    return HookManager::Instance().o_ffxFsr3ConfigureFrameGeneration(context, config);
}

int WINAPI HookManager::hkffxSetFrameGenerationConfigToSwapchainDX12(const FfxFrameGenerationConfigFSR3* config) {
    static int callCount = 0;
    if (config && ++callCount <= 4) LogFgConfig("FG SetConfigToSwapchain (backend)", config);
    if (config) {
        FgConfigDecoded decoded = DecodeFgConfig(config);
        if (decoded.hudLessResource) {
            auto& self = HookManager::Instance();
            {
                std::lock_guard<std::mutex> lock(self.m_upscalerMutex);
                self.m_interceptedFgHudless = (ID3D12Resource*)decoded.hudLessResource;
                self.m_interceptedFgHudlessState = decoded.hudLessState;
            }
            ResourceTracker::Instance().TrackForDestruction((ID3D12Resource*)decoded.hudLessResource);
        }
    }

    return HookManager::Instance().o_ffxSetFrameGenerationConfigToSwapchainDX12(config);
}

// FG-F0 (Path A): the game asks FFX to build its frame-interpolation swapchain. FFX internally calls
// IDXGIFactory::CreateSwapChainForHwnd to make the REAL swapchain — we must let that one through
// UN-wrapped (FFX owns it), otherwise our async proxy + FFX both wrap it => DEVICE_REMOVED. We flag
// the nested window so our CreateSwapChainForHwnd hook stands down.
int WINAPI HookManager::hkffxCreateFrameinterpolationSwapchainForHwndDX12(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, ID3D12CommandQueue* queue, IDXGIFactory* factory, void** outSwapchain) {
    auto& self = HookManager::Instance();
    LOG_INFO("FG CreateFISwapchainForHwnd (Path A): %ux%u fmt=%u — standing our proxy down for FFX's real swapchain",
             desc1 ? desc1->Width : 0, desc1 ? desc1->Height : 0, desc1 ? (unsigned)desc1->Format : 0);
    if (hWnd) s_gameHwnd = hWnd;
    self.m_insideFgSwapchainCreate = true;
    int rc = self.o_ffxCreateFISwapchainForHwndDX12(hWnd, desc1, fsDesc, queue, factory, outSwapchain);
    self.m_insideFgSwapchainCreate = false;
    self.m_fgSwapchainActive = true;
    LOG_INFO("FG CreateFISwapchainForHwnd returned rc=%d  ffxSwapchain=0x%p", rc, outSwapchain ? *outSwapchain : nullptr);
    return rc;
}

// FG-F0 (Path B): the game pre-created a normal swapchain (which we may have wrapped) and now asks
// FFX to replace it with the FG swapchain. Log prominently — if this fires, our proxy is already in
// the chain and we'll need to handle it explicitly (next iteration).
int WINAPI HookManager::hkffxReplaceSwapchainForFrameinterpolationDX12(void* gameQueue, void** gameSwapchain) {
    auto& self = HookManager::Instance();
    LOG_WARN("FG ReplaceSwapchain (Path B) called — game pre-created swapchain 0x%p, FFX wrapping it (our proxy may already be in the chain!)",
             gameSwapchain ? *gameSwapchain : nullptr);
    self.m_fgSwapchainActive = true;
    int rc = self.o_ffxReplaceSwapchainForFIDX12(gameQueue, gameSwapchain);
    LOG_INFO("FG ReplaceSwapchain returned rc=%d  ffxSwapchain=0x%p", rc, gameSwapchain ? *gameSwapchain : nullptr);
    return rc;
}

struct FgDispatchDecoded {
    unsigned int numInterpolatedFrames;
    bool reset;
    void* presentColorResource;
    unsigned int presentColorWidth;
    unsigned int presentColorHeight;
    unsigned int presentColorFormat;
    int presentColorState;
    void* output0Resource;
    unsigned int output0Width;
    unsigned int output0Height;
    unsigned int output0Format;
};

static FgDispatchDecoded DecodeFgDispatch(const void* descPtr) {
    FgDispatchDecoded decoded = {};
    if (!descPtr) return decoded;

    __try {
        const char* base = (const char*)descPtr;
        // FSR 3.0: commandList (8) + presentColor (176) + outputs[4] (704) = 888
        // FSR 3.1: commandList (8) + presentColor (48) + outputs[4] (192) = 248
        uint32_t val31 = *(const uint32_t*)(base + 248);
        bool is31 = (val31 > 0 && val31 < 10);

        if (is31) {
            // FSR 3.1 layout
            decoded.numInterpolatedFrames = val31;
            decoded.reset = *(const bool*)(base + 252);
            
            decoded.presentColorResource = *(void**)(base + 8);
            decoded.presentColorWidth = *(const uint32_t*)(base + 8 + 16);
            decoded.presentColorHeight = *(const uint32_t*)(base + 8 + 20);
            decoded.presentColorFormat = *(const uint32_t*)(base + 8 + 12);
            decoded.presentColorState = *(const int*)(base + 8 + 40);
            
            decoded.output0Resource = *(void**)(base + 56);
            decoded.output0Width = *(const uint32_t*)(base + 56 + 16);
            decoded.output0Height = *(const uint32_t*)(base + 56 + 20);
            decoded.output0Format = *(const uint32_t*)(base + 56 + 12);
        } else {
            // FSR 3.0 layout
            decoded.numInterpolatedFrames = *(const uint32_t*)(base + 888);
            decoded.reset = *(const bool*)(base + 892);
            
            decoded.presentColorResource = *(void**)(base + 8);
            decoded.presentColorWidth = *(const uint32_t*)(base + 8 + 16);
            decoded.presentColorHeight = *(const uint32_t*)(base + 8 + 20);
            decoded.presentColorFormat = *(const uint32_t*)(base + 8 + 12);
            decoded.presentColorState = *(const int*)(base + 8 + 40);
            
            decoded.output0Resource = *(void**)(base + 184);
            decoded.output0Width = *(const uint32_t*)(base + 184 + 16);
            decoded.output0Height = *(const uint32_t*)(base + 184 + 20);
            decoded.output0Format = *(const uint32_t*)(base + 184 + 12);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Safe fallback
    }
    return decoded;
}

// The FG dispatch — the likely path: game generates interpolated frames itself, then presents them.
// Crash-safe probe: inline fields only, no GetDesc.
int WINAPI HookManager::hkffxFsr3DispatchFrameGeneration(const FfxFrameGenerationDispatchDescFSR3* desc) {
    static int callCount = 0;
    if (desc && ++callCount <= 4) {
        __try {
            FgDispatchDecoded decoded = DecodeFgDispatch(desc);
            LOG_INFO("FG DispatchFrameGeneration call #%d: numInterpolated=%u reset=%d  presentColor=0x%p (%ux%u fmt=%u state=%d)  output0=0x%p (%ux%u fmt=%u)",
                     callCount, decoded.numInterpolatedFrames, (int)decoded.reset,
                     decoded.presentColorResource, decoded.presentColorWidth, decoded.presentColorHeight,
                     decoded.presentColorFormat, decoded.presentColorState,
                     decoded.output0Resource, decoded.output0Width, decoded.output0Height,
                     decoded.output0Format);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("FG DispatchFrameGeneration: CRASH AVOIDED in logging!");
        }
    }
    return HookManager::Instance().o_ffxFsr3DispatchFrameGeneration(desc);
}

int WINAPI HookManager::hkffxRegisterFrameinterpolationUiResourceDX12(void* gameSwapChain, FfxResourceFSR3 uiResource, uint32_t flags) {
    auto& self = HookManager::Instance();
    LOG_INFO("hkffxRegisterFrameinterpolationUiResourceDX12 called: swapChain=0x%p, uiResource=0x%p, flags=0x%X",
             gameSwapChain, uiResource.resource, flags);
    if (uiResource.resource) {
        ID3D12Resource* res = (ID3D12Resource*)uiResource.resource;
        LOG_INFO("  Captured FG UI Resource: ptr=0x%p | Size=%ux%u | Format=%u | State=%d",
                 res, uiResource.description.width, uiResource.description.height,
                 uiResource.description.format, uiResource.state);
        {
            std::lock_guard<std::mutex> lock(self.m_upscalerMutex);
            self.m_interceptedFgUi = res;
        }
        ResourceTracker::Instance().TrackForDestruction(res);
    }
    return self.o_ffxRegisterFrameinterpolationUiResourceDX12(gameSwapChain, uiResource, flags);
}

void HookManager::hkffxConfigureHelper(ffxContext* context, const ffxConfigureDescHeader* desc) {
    static uint32_t configureCount = 0;
    if (++configureCount <= 10) {
        LOG_INFO("hkffxConfigure called: context=0x%p, descType=0x%llX", context, desc->type);
    }
    
    // Traverse pNext chain to find UI resources
    const ffxApiHeader* node = (const ffxApiHeader*)desc;
    while (node) {
        if (node->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
            auto uiConfig = (const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*)node;
            if (uiConfig->uiResource.resource) {
                ID3D12Resource* res = (ID3D12Resource*)uiConfig->uiResource.resource;
                LOG_INFO("    Captured FG UI Resource (via ffxConfigure): ptr=0x%p", res);
                {
                    std::lock_guard<std::mutex> lock(m_upscalerMutex);
                    m_interceptedFgUi = res;
                }
                ResourceTracker::Instance().TrackForDestruction(res);
            }
        }
        node = node->pNext;
    }

    // Check main config type for HUDLessColor
    if (desc->type == FFX_API_FRAME_GENERATION_CONFIG) {
        auto config = (const FfxFrameGenerationConfig*)desc;
        if (config->HUDLessColor.resource) {
            ID3D12Resource* res = (ID3D12Resource*)config->HUDLessColor.resource;
            static ID3D12Resource* lastLoggedConfig = nullptr;
            if (res != lastLoggedConfig) {
                LOG_INFO("    Captured HUDLessColor Resource (via ffxConfigure config): ptr=0x%p", res);
                lastLoggedConfig = res;
            }
            {
                std::lock_guard<std::mutex> lock(m_upscalerMutex);
                m_interceptedFgHudless = res;
            }
            ResourceTracker::Instance().TrackForDestruction(res);
        }
    }
    else if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
        auto config = (const ffxConfigureDescFrameGeneration*)desc;
        if (config->HUDLessColor.resource) {
            ID3D12Resource* res = (ID3D12Resource*)config->HUDLessColor.resource;
            static ID3D12Resource* lastLoggedDesc = nullptr;
            if (res != lastLoggedDesc) {
                LOG_INFO("    Captured HUDLessColor Resource (via ffxConfigure desc): ptr=0x%p, FFX State=%u, Format=%u", 
                         res, config->HUDLessColor.state, config->HUDLessColor.description.format);
                lastLoggedDesc = res;
            }
            {
                std::lock_guard<std::mutex> lock(m_upscalerMutex);
                m_interceptedFgHudless = res;
                m_interceptedFgHudlessState = config->HUDLessColor.state;
            }
            ResourceTracker::Instance().TrackForDestruction(res);

        }
    }
    else if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
        auto uiConfig = (const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*)desc;
        if (uiConfig->uiResource.resource) {
            ID3D12Resource* res = (ID3D12Resource*)uiConfig->uiResource.resource;
            LOG_INFO("    Captured FG UI Resource (via direct ffxConfigure): ptr=0x%p", res);
            {
                std::lock_guard<std::mutex> lock(m_upscalerMutex);
                m_interceptedFgUi = res;
            }
            ResourceTracker::Instance().TrackForDestruction(res);
        }
    }
}

ffxReturnCode_t WINAPI HookManager::hkffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    auto& self = HookManager::Instance();
    
    if (desc) {
        __try {
            self.hkffxConfigureHelper(context, desc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("hkffxConfigure: CRASH AVOIDED in configure intercept!");
        }
    }

    return self.o_ffxConfigure(context, desc);
}

ffxReturnCode_t WINAPI HookManager::hkffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    auto& self = HookManager::Instance();
    if (desc) {
        static uint32_t dispatchCount = 0;
        if (++dispatchCount <= 10) {
            LOG_INFO("hkffxDispatch called: context=0x%p, descType=0x%llX", context, desc->type);
        }
    }
    ffxReturnCode_t ret = self.o_ffxDispatch(context, desc);
    if (desc && desc->type == 0x00020003u) { // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION
        ID3D12Resource* fgHudless = self.GetInterceptedFgHudless();
        if (fgHudless) {
            auto fgDesc = (const ffxDispatchDescFrameGeneration*)desc;
            auto* cmdList = (ID3D12GraphicsCommandList*)fgDesc->commandList;
            if (cmdList) {
                ExportManager::Instance().CopyFgHudlessOnDispatch(cmdList, fgHudless, self.GetInterceptedFgHudlessState());
            }
        }
    }

    return ret;
}

void HookManager::HookFGBackend(HMODULE hMod) {
    // Read-only: which FG-backend symbols does THIS module export?
    const char* syms[] = {
        "ffxCreateFrameinterpolationSwapchainForHwndDX12",
        "ffxReplaceSwapchainForFrameinterpolationDX12",
        "ffxSetFrameGenerationConfigToSwapchainDX12",
        "ffxCreateFrameinterpolationSwapchainDX12",
        "ffxRegisterFrameinterpolationUiResourceDX12",
        "ffxConfigure",
        "ffxDispatch",
    };
    for (const char* s : syms)
        LOG_INFO("  FG backend export: %-50s %s", s, GetProcAddress(hMod, s) ? "PRESENT" : "absent");

    auto attach = [](PVOID& orig, PVOID hook, const char* name) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&orig, hook);
        DetourTransactionCommit();
        LOG_INFO("Detours: %s hooked successfully.", name);
    };

    if (!o_ffxCreateFISwapchainForHwndDX12) {
        if (auto p = (PFN_ffxCreateFISwapchainForHwndDX12)GetProcAddress(hMod, "ffxCreateFrameinterpolationSwapchainForHwndDX12")) {
            o_ffxCreateFISwapchainForHwndDX12 = p;
            attach((PVOID&)o_ffxCreateFISwapchainForHwndDX12, hkffxCreateFrameinterpolationSwapchainForHwndDX12, "FG ffxCreateFrameinterpolationSwapchainForHwndDX12");
        }
    }
    if (!o_ffxReplaceSwapchainForFIDX12) {
        if (auto p = (PFN_ffxReplaceSwapchainForFIDX12)GetProcAddress(hMod, "ffxReplaceSwapchainForFrameinterpolationDX12")) {
            o_ffxReplaceSwapchainForFIDX12 = p;
            attach((PVOID&)o_ffxReplaceSwapchainForFIDX12, hkffxReplaceSwapchainForFrameinterpolationDX12, "FG ffxReplaceSwapchainForFrameinterpolationDX12");
        }
    }
    if (!o_ffxSetFrameGenerationConfigToSwapchainDX12) {
        if (auto p = (PFN_ffxSetFrameGenerationConfigToSwapchainDX12)GetProcAddress(hMod, "ffxSetFrameGenerationConfigToSwapchainDX12")) {
            o_ffxSetFrameGenerationConfigToSwapchainDX12 = p;
            attach((PVOID&)o_ffxSetFrameGenerationConfigToSwapchainDX12, hkffxSetFrameGenerationConfigToSwapchainDX12, "FG ffxSetFrameGenerationConfigToSwapchainDX12");
        }
    }
    if (!o_ffxRegisterFrameinterpolationUiResourceDX12) {
        if (auto p = (PFN_ffxRegisterFrameinterpolationUiResourceDX12)GetProcAddress(hMod, "ffxRegisterFrameinterpolationUiResourceDX12")) {
            o_ffxRegisterFrameinterpolationUiResourceDX12 = p;
            attach((PVOID&)o_ffxRegisterFrameinterpolationUiResourceDX12, hkffxRegisterFrameinterpolationUiResourceDX12, "FG ffxRegisterFrameinterpolationUiResourceDX12");
        }
    }
    if (!o_ffxConfigure) {
        if (auto p = (PFN_ffxConfigure)GetProcAddress(hMod, "ffxConfigure")) {
            o_ffxConfigure = p;
            attach((PVOID&)o_ffxConfigure, hkffxConfigure, "FG ffxConfigure");
        }
    }
    if (!o_ffxDispatch) {
        if (auto p = (PFN_ffxDispatch)GetProcAddress(hMod, "ffxDispatch")) {
            o_ffxDispatch = p;
            attach((PVOID&)o_ffxDispatch, hkffxDispatch, "FG ffxDispatch");
        }
    }
}

void HookManager::CheckAndHookUpscaler(LPCWSTR lpLibFileName, HMODULE hMod) {
    if (!lpLibFileName || !hMod) return;
    std::wstring name(lpLibFileName);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);

    if (name.find(L"nvngx.dll") != std::wstring::npos) {
        HookNVNGX(hMod);
    } else if (name.find(L"ffx_fsr2_api_dx12_x64.dll") != std::wstring::npos ||
               name.find(L"ffx_fsr2_api_dx12.dll") != std::wstring::npos) {
        HookFSR2(hMod);
    } else if (name.find(L"ffx_frameinterpolation") != std::wstring::npos ||
               name.find(L"ffx_backend_dx12") != std::wstring::npos ||
               name.find(L"amd_fidelityfx_dx12") != std::wstring::npos ||
               name.find(L"ffx_api_dx12") != std::wstring::npos) {
        HookFGBackend(hMod);
    } else if (name.find(L"ffx_fsr3") != std::wstring::npos) {
        HookFSR3(hMod);
    }
}

void HookManager::HookNVNGX(HMODULE hMod) {
    if (o_NVSDK_NGX_D3D12_EvaluateFeature) return; // Already hooked

    auto pEval = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(hMod, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (pEval) {
        o_NVSDK_NGX_D3D12_EvaluateFeature = pEval;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)o_NVSDK_NGX_D3D12_EvaluateFeature, hkNVSDK_NGX_D3D12_EvaluateFeature);
        DetourTransactionCommit();
        LOG_INFO("Detours: nvngx.dll EvaluateFeature hooked successfully.");
    }
}

void HookManager::HookFSR2(HMODULE hMod) {
    if (o_ffxFsr2ContextEvaluate) return; // Already hooked

    auto pEval = (PFN_ffxFsr2ContextEvaluate)GetProcAddress(hMod, "ffxFsr2ContextEvaluate");
    if (pEval) {
        o_ffxFsr2ContextEvaluate = pEval;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)o_ffxFsr2ContextEvaluate, hkffxFsr2ContextEvaluate);
        DetourTransactionCommit();
        LOG_INFO("Detours: FSR2 ffxFsr2ContextEvaluate hooked successfully.");
    }
}

void HookManager::HookFSR3(HMODULE hMod) {
    // 1. Hook ffxFsr3ContextDispatchUpscale
    if (!o_ffxFsr3ContextDispatchUpscale) {
        auto pEval = (PFN_ffxFsr3ContextDispatchUpscale)GetProcAddress(hMod, "ffxFsr3ContextDispatchUpscale");
        if (pEval) {
            o_ffxFsr3ContextDispatchUpscale = pEval;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxFsr3ContextDispatchUpscale, hkffxFsr3ContextDispatchUpscale);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxFsr3ContextDispatchUpscale hooked successfully.");
        }
    }

    // 2. Hook ffxFsr3UpscalerContextDispatch
    if (!o_ffxFsr3UpscalerContextDispatch) {
        auto pEval = (PFN_ffxFsr3UpscalerContextDispatch)GetProcAddress(hMod, "ffxFsr3UpscalerContextDispatch");
        if (pEval) {
            o_ffxFsr3UpscalerContextDispatch = pEval;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxFsr3UpscalerContextDispatch, hkffxFsr3UpscalerContextDispatch);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxFsr3UpscalerContextDispatch hooked successfully.");
        }
    }

    // 3. Hook ffxFsr3ContextEvaluate (compatibility)
    if (!o_ffxFsr3ContextEvaluate) {
        auto pEval = (PFN_ffxFsr3ContextEvaluate)GetProcAddress(hMod, "ffxFsr3ContextEvaluate");
        if (!pEval) {
            pEval = (PFN_ffxFsr3ContextEvaluate)GetProcAddress(hMod, "ffxFsr3UpscalerContextEvaluate");
        }
        if (pEval) {
            o_ffxFsr3ContextEvaluate = pEval;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxFsr3ContextEvaluate, hkffxFsr3ContextEvaluate);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxFsr3ContextEvaluate hooked successfully.");
        }
    }

    // 4. Hook ffxFsr3ConfigureFrameGeneration (FG-config probe; absent if the game uses the FSR3.1
    // ffx-api FG path — that's fine, we just won't attach and log nothing).
    if (!o_ffxFsr3ConfigureFrameGeneration) {
        auto pCfg = (PFN_ffxFsr3ConfigureFrameGeneration)GetProcAddress(hMod, "ffxFsr3ConfigureFrameGeneration");
        if (pCfg) {
            o_ffxFsr3ConfigureFrameGeneration = pCfg;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxFsr3ConfigureFrameGeneration, hkffxFsr3ConfigureFrameGeneration);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxFsr3ConfigureFrameGeneration hooked successfully.");
        } else {
            LOG_INFO("FSR3 ffxFsr3ConfigureFrameGeneration not exported by this module (FG may use ffx-api path).");
        }
    }

    // 4b. Hook the FG dispatch (game-generates-frames path). Exported from the FSR3 module.
    if (!o_ffxFsr3DispatchFrameGeneration) {
        auto p = (PFN_ffxFsr3DispatchFrameGeneration)GetProcAddress(hMod, "ffxFsr3DispatchFrameGeneration");
        LOG_INFO("  FG dispatch probe: ffxFsr3DispatchFrameGeneration %s", p ? "PRESENT" : "absent");
        if (p) {
            o_ffxFsr3DispatchFrameGeneration = p;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxFsr3DispatchFrameGeneration, hkffxFsr3DispatchFrameGeneration);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxFsr3DispatchFrameGeneration hooked successfully.");
        }
    }

    // 5. Probe which FG-backend symbols this module exports (read-only) — decisive for whether FG is
    // even present/engaging under our injection, and which entry point to integrate with.
    const char* fgSyms[] = {
        "ffxSetFrameGenerationConfigToSwapchainDX12",
        "ffxCreateFrameinterpolationSwapchainForHwndDX12",
        "ffxCreateFrameinterpolationSwapchainDX12",
        "ffxReplaceSwapchainForFrameinterpolationDX12",
        "ffxRegisterFrameinterpolationUiResourceDX12",
        "ffxGetFrameinterpolationTextureDX12",
    };
    for (const char* s : fgSyms)
        LOG_INFO("  FG export probe: %-50s %s", s, GetProcAddress(hMod, s) ? "PRESENT" : "absent");

    // Hook the backend config-to-swapchain (what the high-level wrapper forwards to; the game may call
    // it directly). If FG configures at all, this fires even when the high-level one doesn't.
    if (!o_ffxSetFrameGenerationConfigToSwapchainDX12) {
        auto p = (PFN_ffxSetFrameGenerationConfigToSwapchainDX12)GetProcAddress(hMod, "ffxSetFrameGenerationConfigToSwapchainDX12");
        if (p) {
            o_ffxSetFrameGenerationConfigToSwapchainDX12 = p;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxSetFrameGenerationConfigToSwapchainDX12, hkffxSetFrameGenerationConfigToSwapchainDX12);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxSetFrameGenerationConfigToSwapchainDX12 hooked successfully.");
        }
    }
    if (!o_ffxRegisterFrameinterpolationUiResourceDX12) {
        auto p = (PFN_ffxRegisterFrameinterpolationUiResourceDX12)GetProcAddress(hMod, "ffxRegisterFrameinterpolationUiResourceDX12");
        if (p) {
            o_ffxRegisterFrameinterpolationUiResourceDX12 = p;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)o_ffxRegisterFrameinterpolationUiResourceDX12, hkffxRegisterFrameinterpolationUiResourceDX12);
            DetourTransactionCommit();
            LOG_INFO("Detours: FSR3 ffxRegisterFrameinterpolationUiResourceDX12 hooked successfully.");
        }
    }
}

ID3D12Resource* HookManager::GetInterceptedColor() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedColor;
}

ID3D12Resource* HookManager::GetInterceptedDepth() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedDepth;
}

ID3D12Resource* HookManager::GetInterceptedMV() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedMV;
}

ID3D12Resource* HookManager::GetInterceptedFgHudless() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedFgHudless;
}

uint32_t HookManager::GetInterceptedFgHudlessState() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedFgHudlessState;
}

ID3D12Resource* HookManager::GetInterceptedFgUi() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    return m_interceptedFgUi;
}

void HookManager::ClearInterceptedResources() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    m_interceptedColor = nullptr;
    m_interceptedDepth = nullptr;
    m_interceptedMV = nullptr;
    m_interceptedFgHudless = nullptr;
    m_interceptedFgHudlessState = 0;
    m_interceptedFgUi = nullptr;
}

bool HookManager::ConsumeDispatchFlag() {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    bool dispatched = m_dispatchOccurredThisFrame;
    m_dispatchOccurredThisFrame = false;
    return dispatched;
}

void HookManager::OnResourceDestroyed(ID3D12Resource* resource) {
    std::lock_guard<std::mutex> lock(m_upscalerMutex);
    if (m_interceptedColor == resource) {
        m_interceptedColor = nullptr;
    }
    if (m_interceptedDepth == resource) {
        m_interceptedDepth = nullptr;
    }
    if (m_interceptedMV == resource) {
        m_interceptedMV = nullptr;
    }
    if (m_interceptedFgHudless == resource) {
        m_interceptedFgHudless = nullptr;
    }
    if (m_interceptedFgUi == resource) {
        m_interceptedFgUi = nullptr;
    }
}
