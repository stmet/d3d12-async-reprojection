#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include "../ipc/shared_metadata.h"

struct ExportSlot {
    ID3D12Resource* colorTex = nullptr;
    ID3D12Resource* depthTex = nullptr;
    ID3D12Resource* mvTex = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t currentFenceValue = 0;
    HANDLE fenceEvent = nullptr;
};

class ExportManager {
public:
    static ExportManager& Instance();

    void SetDevice(ID3D12Device* device);
    void SetActiveCommandQueue(ID3D12CommandQueue* queue);
    void InitializeSwapChain(IDXGISwapChain* swapchain);
    
    void OnPresent(IDXGISwapChain* swapchain);

private:
    ExportManager();
    ~ExportManager();

    bool InitializeIPC();
    bool RecreateExportTextures(uint32_t width, uint32_t height, DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvFmt);
    void ReleaseTextures();

    void CopyResourceToExport(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* src, ID3D12Resource* dst);

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12CommandAllocator* m_cmdAllocators[3] = { nullptr, nullptr, nullptr };
    ID3D12GraphicsCommandList* m_cmdList = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_mvFormat = DXGI_FORMAT_UNKNOWN;

    uint64_t m_frameId = 0;
    uint64_t m_sequenceNumber = 0;

    ExportSlot m_slots[3];
    SharedRingBuffer* m_sharedMetadata = nullptr;
    HANDLE m_fileMapping = NULL;

    std::mutex m_mutex;
    bool m_ipcInitialized = false;
    bool m_texturesInitialized = false;
};
