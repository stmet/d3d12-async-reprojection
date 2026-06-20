#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

// Lean hook manager: detours only IDXGIFactory::CreateSwapChain(ForHwnd) so we can hand the game our
// proxy swapchain. Present is NOT detoured — the game holds our proxy and calls its vtable directly,
// and the presenter calls the real Present through the extracted o_Present pointer (CallRealPresent).
// No upscaler/FG hooks: the lean branch runs with FSR Frame Generation OFF and does its own
// reprojection-based frame generation, so it needs nothing from FSR/FFX.
class HookManager {
public:
    static HookManager& Instance();
    bool InstallHooks();
    void UninstallHooks();

    // The game's render window, captured at swapchain creation (used by the mouse injector).
    static HWND GetGameHwnd();

    // Invoke the real (un-detoured) Present/Present1 via the extracted vtable pointer. Used by the
    // proxy/presenter to present the real swapchain without re-entering any hook.
    static HRESULT CallRealPresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags);
    static HRESULT CallRealPresent1(IDXGISwapChain1* sc, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);

private:
    HookManager() = default;
    ~HookManager() = default;

    bool InitializeVTables();

    typedef HRESULT(WINAPI* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT(WINAPI* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(WINAPI* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

    static HRESULT WINAPI hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
    static HRESULT WINAPI hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

    PFN_CreateSwapChain        o_CreateSwapChain = nullptr;
    PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
    PFN_Present                o_Present = nullptr;   // extracted, not detoured
    PFN_Present1               o_Present1 = nullptr;  // extracted, not detoured

    bool m_initialized = false;
    bool m_hooksInstalled = false;

    static HWND s_gameHwnd;
};
