#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <unordered_map>
#include <mutex>

struct TrackedResource {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc = {};
    uint64_t lastFrameActive = 0;
    uint32_t bindCountThisFrame = 0;
    uint32_t transitionCountThisFrame = 0;
    uint32_t dsvBindCount = 0;
    uint32_t rtvBindCount = 0;
    
    float depthScore = 0.0f;
    float mvScore = 0.0f;
    float colorScore = 0.0f;
    
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
};

class ResourceTracker {
public:
    static ResourceTracker& Instance();

    void RegisterResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC* desc);
    void UnregisterResource(ID3D12Resource* resource);
    
    void RegisterDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource);
    ID3D12Resource* GetResourceFromDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);

    void OnResourceTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void OnOMSetRenderTargets(ID3D12GraphicsCommandList* cmdList, UINT numRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandles, BOOL singleHandleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle);

    D3D12_RESOURCE_STATES GetResourceState(ID3D12Resource* resource);

    void EndFrame(uint64_t frameId, uint32_t width, uint32_t height);
    
    ID3D12Resource* GetBestColorCandidate();
    ID3D12Resource* GetBestDepthCandidate();
    ID3D12Resource* GetBestMVCandidate();

    void DumpDebugScores();

private:
    ResourceTracker() = default;
    ~ResourceTracker() = default;

    std::unordered_map<ID3D12Resource*, TrackedResource> m_resources;
    std::unordered_map<SIZE_T, ID3D12Resource*> m_descriptors;
    std::mutex m_mutex;

    ID3D12Resource* m_bestColor = nullptr;
    ID3D12Resource* m_bestDepth = nullptr;
    ID3D12Resource* m_bestMV = nullptr;
    uint64_t m_lastDumpFrame = 0;
};
