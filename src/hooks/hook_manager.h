#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

class HookManager {
public:
    static HookManager& Instance();
    bool InstallHooks();
    void UninstallHooks();

private:
    HookManager() = default;
    ~HookManager() = default;

    bool InitializeVTables();

    // Original function pointers
    typedef HRESULT(WINAPI* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT(WINAPI* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(WINAPI* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT(WINAPI* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    typedef HRESULT(WINAPI* PFN_CreatePlacedResource)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);

    typedef void(WINAPI* PFN_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    typedef void(WINAPI* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    typedef void(WINAPI* PFN_CreateShaderResourceView)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    typedef void(WINAPI* PFN_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    typedef void(WINAPI* PFN_CreateDepthStencilView)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);

    // Hooks
    static HRESULT WINAPI hkCreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
    static HRESULT WINAPI hkCreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);
    static HRESULT WINAPI hkPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
    static HRESULT WINAPI hkPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    static HRESULT WINAPI hkCreateCommittedResource(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidGuid, void** ppvResource);
    static HRESULT WINAPI hkCreatePlacedResource(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidGuid, void** ppvResource);

    static void WINAPI hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers);
    static void WINAPI hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
    static void WINAPI hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static void WINAPI hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static void WINAPI hkCreateDepthStencilView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

    // Member pointers
    PFN_CreateSwapChain o_CreateSwapChain = nullptr;
    PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
    PFN_Present o_Present = nullptr;
    PFN_Present1 o_Present1 = nullptr;
    PFN_CreateCommittedResource o_CreateCommittedResource = nullptr;
    PFN_CreatePlacedResource o_CreatePlacedResource = nullptr;

    PFN_ResourceBarrier o_ResourceBarrier = nullptr;
    PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
    PFN_CreateShaderResourceView o_CreateShaderResourceView = nullptr;
    PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
    PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;

    bool m_initialized = false;
    bool m_hooksInstalled = false;

    friend class HookManagerHelper;
};
