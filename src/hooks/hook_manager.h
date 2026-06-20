#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include "upscaler_defs.h"

class HookManager {
public:
    static HookManager& Instance();
    bool InstallHooks();
    void UninstallHooks();

    // Accessors for intercepted resources
    ID3D12Resource* GetInterceptedColor();
    ID3D12Resource* GetInterceptedDepth();
    ID3D12Resource* GetInterceptedMV();
    ID3D12Resource* GetInterceptedFgHudless();
    uint32_t GetInterceptedFgHudlessState();
    ID3D12Resource* GetInterceptedFgUi();
    void ClearInterceptedResources();
    void OnResourceDestroyed(ID3D12Resource* resource);

    // Returns true if an FSR3 dispatch was intercepted since the last call, and resets
    // the flag. Used by ExportManager to detect "extra" presents (e.g. frame-generation
    // interpolated frames) that have no fresh upscaler inputs.
    bool ConsumeDispatchFlag();

    void CheckAndHookUpscaler(LPCWSTR lpLibFileName, HMODULE hMod);

    // The game's render window, captured at swapchain creation. Used by the input injector
    // to PostMessage(WM_INPUT) at the window whose thread owns the raw-input dispatch.
    static HWND GetGameHwnd();

    // Invoke the real (un-hooked) Present/Present1 via the Detours trampoline. Used by the proxy
    // swapchain to present without re-entering hkPresent (which would double-process the frame).
    static HRESULT CallRealPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags);
    static HRESULT CallRealPresent1(IDXGISwapChain1* sc, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);

private:
    HookManager() = default;
    ~HookManager() = default;

    bool InitializeVTables();

    void HookNVNGX(HMODULE hMod);
    void HookFSR2(HMODULE hMod);
    void HookFSR3(HMODULE hMod);
    void HookFGBackend(HMODULE hMod);   // ffx_frameinterpolation_x64 / ffx_backend_dx12 / amd_fidelityfx_dx12

    // Original function pointers
    typedef HRESULT(WINAPI* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT(WINAPI* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(WINAPI* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

    // Dynamic library hooks
    typedef HMODULE(WINAPI* PFN_LoadLibraryW)(LPCWSTR);
    typedef HMODULE(WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
    typedef HMODULE(WINAPI* PFN_LoadLibraryA)(LPCSTR);
    typedef HMODULE(WINAPI* PFN_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
    typedef int(WINAPI* PFN_NVSDK_NGX_D3D12_EvaluateFeature)(ID3D12GraphicsCommandList*, const void*, const NVSDK_NGX_Parameter*, void*);
    typedef int(WINAPI* PFN_ffxFsr2ContextEvaluate)(void*, const FfxFsr2DispatchDescription*);
    typedef int(WINAPI* PFN_ffxFsr3ContextEvaluate)(void*, const FfxFsr3DispatchDescription*);
    typedef int(WINAPI* PFN_ffxFsr3ContextDispatchUpscale)(void*, const FfxFsr3DispatchDescription*);
    typedef int(WINAPI* PFN_ffxFsr3UpscalerContextDispatch)(void*, const FfxFsr3DispatchDescription*);
    typedef int(WINAPI* PFN_ffxFsr3ConfigureFrameGeneration)(void*, const FfxFrameGenerationConfigFSR3*);
    typedef int(WINAPI* PFN_ffxSetFrameGenerationConfigToSwapchainDX12)(const FfxFrameGenerationConfigFSR3*);
    // FfxSwapchain / FfxCommandQueue are opaque void* handles; FfxSwapchain& -> void**.
    typedef int(WINAPI* PFN_ffxCreateFISwapchainForHwndDX12)(HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory*, void**);
    typedef int(WINAPI* PFN_ffxReplaceSwapchainForFIDX12)(void*, void**);
    typedef int(WINAPI* PFN_ffxFsr3DispatchFrameGeneration)(const FfxFrameGenerationDispatchDescFSR3*);
    typedef int(WINAPI* PFN_ffxRegisterFrameinterpolationUiResourceDX12)(void*, FfxResourceFSR3, uint32_t);
    typedef ffxReturnCode_t(WINAPI* PFN_ffxConfigure)(ffxContext*, const ffxConfigureDescHeader*);
    typedef ffxReturnCode_t(WINAPI* PFN_ffxDispatch)(ffxContext*, const ffxDispatchDescHeader*);

    // Hooks
    static HRESULT WINAPI hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
    static HRESULT WINAPI hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
    static HRESULT WINAPI hkPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
    static HRESULT WINAPI hkPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

    static HMODULE WINAPI hkLoadLibraryW(LPCWSTR lpLibFileName);
    static HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
    static HMODULE WINAPI hkLoadLibraryA(LPCSTR lpLibFileName);
    static HMODULE WINAPI hkLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
    static int WINAPI hkNVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList, const void* InFeatureHandle, const NVSDK_NGX_Parameter* InParameters, void* InCallback);
    static int WINAPI hkffxFsr2ContextEvaluate(void* context, const FfxFsr2DispatchDescription* dispatchDescription);
    static int WINAPI hkffxFsr3ContextEvaluate(void* context, const FfxFsr3DispatchDescription* dispatchDescription);
    static int WINAPI hkffxFsr3ContextDispatchUpscale(void* context, const FfxFsr3DispatchDescription* dispatchDescription);
    static int WINAPI hkffxFsr3UpscalerContextDispatch(void* context, const FfxFsr3DispatchDescription* dispatchDescription);
    // FG-config probe (step 1 of the hud-less pivot): logs whether the game supplies HUDLessColor or
    // uses the present (UI) callback. Read-only — never dereferences resource pointers.
    static int WINAPI hkffxFsr3ConfigureFrameGeneration(void* context, const FfxFrameGenerationConfigFSR3* config);
    static int WINAPI hkffxSetFrameGenerationConfigToSwapchainDX12(const FfxFrameGenerationConfigFSR3* config);
    // FG-F0: intercept FFX's frame-interpolation swapchain creation so our async proxy stands down
    // (FFX must own the real swapchain — both wrapping it = device-removed crash).
    static int WINAPI hkffxCreateFrameinterpolationSwapchainForHwndDX12(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, ID3D12CommandQueue* queue, IDXGIFactory* factory, void** outSwapchain);
    static int WINAPI hkffxReplaceSwapchainForFrameinterpolationDX12(void* gameQueue, void** gameSwapchain);
    // The FG dispatch — fires when the game generates interpolated frames itself (the likely path).
    static int WINAPI hkffxFsr3DispatchFrameGeneration(const FfxFrameGenerationDispatchDescFSR3* desc);
    static int WINAPI hkffxRegisterFrameinterpolationUiResourceDX12(void* gameSwapChain, FfxResourceFSR3 uiResource, uint32_t flags);
    static ffxReturnCode_t WINAPI hkffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc);
    static ffxReturnCode_t WINAPI hkffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc);

    // Shared body for the FSR3 dispatch hooks: store intercepted pointers and record the
    // depth/MV copy onto the game's command list.
    static void HandleFsr3Dispatch(const FfxFsr3DispatchDescription* dispatchDescription);
    void hkffxConfigureHelper(ffxContext* context, const ffxConfigureDescHeader* desc);

    // Member pointers
    PFN_CreateSwapChain o_CreateSwapChain = nullptr;
    PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
    PFN_Present o_Present = nullptr;
    PFN_Present1 o_Present1 = nullptr;

    PFN_LoadLibraryW o_LoadLibraryW = nullptr;
    PFN_LoadLibraryExW o_LoadLibraryExW = nullptr;
    PFN_LoadLibraryA o_LoadLibraryA = nullptr;
    PFN_LoadLibraryExA o_LoadLibraryExA = nullptr;
    PFN_NVSDK_NGX_D3D12_EvaluateFeature o_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
    PFN_ffxFsr2ContextEvaluate o_ffxFsr2ContextEvaluate = nullptr;
    PFN_ffxFsr3ContextEvaluate o_ffxFsr3ContextEvaluate = nullptr;
    PFN_ffxFsr3ContextDispatchUpscale o_ffxFsr3ContextDispatchUpscale = nullptr;
    PFN_ffxFsr3UpscalerContextDispatch o_ffxFsr3UpscalerContextDispatch = nullptr;
    PFN_ffxFsr3ConfigureFrameGeneration o_ffxFsr3ConfigureFrameGeneration = nullptr;
    PFN_ffxSetFrameGenerationConfigToSwapchainDX12 o_ffxSetFrameGenerationConfigToSwapchainDX12 = nullptr;
    PFN_ffxCreateFISwapchainForHwndDX12 o_ffxCreateFISwapchainForHwndDX12 = nullptr;
    PFN_ffxReplaceSwapchainForFIDX12 o_ffxReplaceSwapchainForFIDX12 = nullptr;
    PFN_ffxFsr3DispatchFrameGeneration o_ffxFsr3DispatchFrameGeneration = nullptr;
    PFN_ffxRegisterFrameinterpolationUiResourceDX12 o_ffxRegisterFrameinterpolationUiResourceDX12 = nullptr;
    PFN_ffxConfigure o_ffxConfigure = nullptr;
    PFN_ffxDispatch o_ffxDispatch = nullptr;

    // FG-F0 state: set while FFX is creating its FG swapchain (so our nested CreateSwapChainForHwnd
    // hook passes the real swapchain through un-wrapped), and once FG owns presentation.
    bool m_insideFgSwapchainCreate = false;
    bool m_fgSwapchainActive = false;

    // Intercepted resources
    ID3D12Resource* m_interceptedColor = nullptr;
    ID3D12Resource* m_interceptedDepth = nullptr;
    ID3D12Resource* m_interceptedMV = nullptr;
    ID3D12Resource* m_interceptedFgHudless = nullptr;
    uint32_t        m_interceptedFgHudlessState = 0;
    ID3D12Resource* m_interceptedFgUi = nullptr;
    bool m_dispatchOccurredThisFrame = false;
    std::mutex m_upscalerMutex;

    bool m_initialized = false;
    bool m_hooksInstalled = false;

    static HWND s_gameHwnd;

    friend class HookManagerHelper;
};
