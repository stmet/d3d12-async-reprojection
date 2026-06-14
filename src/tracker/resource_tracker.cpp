#include "resource_tracker.h"
#include "../common/logger.h"

ResourceTracker& ResourceTracker::Instance() {
    static ResourceTracker instance;
    return instance;
}

void ResourceTracker::RegisterResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC* desc) {
    if (!resource || !desc) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;

    TrackedResource tracked = {};
    tracked.resource = resource;
    tracked.desc = *desc;
    
    m_resources[resource] = tracked;
}

void ResourceTracker::UnregisterResource(ID3D12Resource* resource) {
    if (!resource) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_resources.erase(resource);

    for (auto it = m_descriptors.begin(); it != m_descriptors.end(); ) {
        if (it->second == resource) {
            it = m_descriptors.erase(it);
        } else {
            ++it;
        }
    }
}

void ResourceTracker::RegisterDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource) {
    if (!resource) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_descriptors[handle.ptr] = resource;
}

ID3D12Resource* ResourceTracker::GetResourceFromDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_descriptors.find(handle.ptr);
    if (it != m_descriptors.end()) {
        return it->second;
    }
    return nullptr;
}

void ResourceTracker::OnResourceTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_resources.find(resource);
    if (it != m_resources.end()) {
        it->second.transitionCountThisFrame++;
        it->second.lastFrameActive = 1;
        it->second.currentState = after;
    }
}

D3D12_RESOURCE_STATES ResourceTracker::GetResourceState(ID3D12Resource* resource) {
    if (!resource) return D3D12_RESOURCE_STATE_COMMON;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_resources.find(resource);
    if (it != m_resources.end()) {
        return it->second.currentState;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

void ResourceTracker::OnOMSetRenderTargets(ID3D12GraphicsCommandList* cmdList, UINT numRTVs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandles, BOOL singleHandleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (dsvHandle) {
        auto it = m_descriptors.find(dsvHandle->ptr);
        if (it != m_descriptors.end()) {
            auto resIt = m_resources.find(it->second);
            if (resIt != m_resources.end()) {
                resIt->second.dsvBindCount++;
                resIt->second.bindCountThisFrame++;
            }
        }
    }

    if (rtvHandles && numRTVs > 0) {
        auto it = m_descriptors.find(rtvHandles[0].ptr);
        if (it != m_descriptors.end()) {
            auto resIt = m_resources.find(it->second);
            if (resIt != m_resources.end()) {
                resIt->second.rtvBindCount++;
                resIt->second.bindCountThisFrame++;
            }
        }
    }
}

void ResourceTracker::EndFrame(uint64_t frameId, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(m_mutex);

    ID3D12Resource* bestDepth = nullptr;
    float maxDepthScore = 0.0f;

    ID3D12Resource* bestMV = nullptr;
    float maxMVScore = 0.0f;

    ID3D12Resource* bestColor = nullptr;
    float maxColorScore = 0.0f;

    for (auto& pair : m_resources) {
        auto& res = pair.second;
        auto desc = res.desc;

        res.depthScore = 0.0f;
        res.mvScore = 0.0f;
        res.colorScore = 0.0f;

        bool matchesResolution = (desc.Width == width && desc.Height == height);
        bool reasonableResolution = (desc.Width >= width / 2 && desc.Width <= width * 2 &&
                                     desc.Height >= height / 2 && desc.Height <= height * 2);

        bool isDepthFormat = (desc.Format == DXGI_FORMAT_D32_FLOAT ||
                              desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                              desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                              desc.Format == DXGI_FORMAT_D16_UNORM ||
                              desc.Format == DXGI_FORMAT_R32_TYPELESS ||
                              desc.Format == DXGI_FORMAT_R24G8_TYPELESS);

        if (isDepthFormat) res.depthScore += 50.0f;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) res.depthScore += 30.0f;
        if (res.dsvBindCount > 0) res.depthScore += 20.0f;
        if (matchesResolution) res.depthScore += 20.0f;
        else if (reasonableResolution) res.depthScore += 10.0f;

        bool isMVFormat = (desc.Format == DXGI_FORMAT_R16G16_FLOAT ||
                           desc.Format == DXGI_FORMAT_R16G16_UNORM ||
                           desc.Format == DXGI_FORMAT_R16G16_SNORM ||
                           desc.Format == DXGI_FORMAT_R32G32_FLOAT);

        if (isMVFormat) res.mvScore += 50.0f;
        if (matchesResolution) res.mvScore += 30.0f;
        else if (reasonableResolution) res.mvScore += 15.0f;
        if (res.transitionCountThisFrame > 0) res.mvScore += 20.0f;
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) res.mvScore += 10.0f;

        bool isColorFormat = (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                              desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (isColorFormat) res.colorScore += 40.0f;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) res.colorScore += 30.0f;
        if (matchesResolution) res.colorScore += 20.0f;
        if (res.rtvBindCount > 0) res.colorScore += 10.0f;

        if (res.depthScore > maxDepthScore) {
            maxDepthScore = res.depthScore;
            bestDepth = res.resource;
        }
        if (res.mvScore > maxMVScore) {
            maxMVScore = res.mvScore;
            bestMV = res.resource;
        }
        if (res.colorScore > maxColorScore) {
            maxColorScore = res.colorScore;
            bestColor = res.resource;
        }

        res.bindCountThisFrame = 0;
        res.transitionCountThisFrame = 0;
        res.dsvBindCount = 0;
        res.rtvBindCount = 0;
    }

    m_bestColor = bestColor;
    m_bestDepth = bestDepth;
    m_bestMV = bestMV;

    if (frameId % 120 == 0) {
        m_lastDumpFrame = frameId;
        DumpDebugScores();
    }
}

ID3D12Resource* ResourceTracker::GetBestColorCandidate() {
    return m_bestColor;
}

ID3D12Resource* ResourceTracker::GetBestDepthCandidate() {
    return m_bestDepth;
}

ID3D12Resource* ResourceTracker::GetBestMVCandidate() {
    return m_bestMV;
}

void ResourceTracker::DumpDebugScores() {
    LOG_INFO("=== Resource Tracker Candidate Scoring Dump (Frame %llu) ===", m_lastDumpFrame);
    LOG_INFO("Total tracked resources: %zu", m_resources.size());
    
    for (const auto& pair : m_resources) {
        const auto& res = pair.second;
        LOG_INFO("Resource [0x%p]: Dim=%ux%u, Format=%u, Flags=0x%X",
            res.resource,
            (uint32_t)res.desc.Width,
            res.desc.Height,
            (uint32_t)res.desc.Format,
            (uint32_t)res.desc.Flags
        );
        LOG_INFO("  -> Color Score: %.1f | Depth Score: %.1f | MV Score: %.1f",
            res.colorScore, res.depthScore, res.mvScore
        );
    }
    
    LOG_INFO("Best Candidates selected:");
    LOG_INFO("  -> Color: 0x%p", m_bestColor);
    LOG_INFO("  -> Depth: 0x%p", m_bestDepth);
    LOG_INFO("  -> Motion Vector: 0x%p", m_bestMV);
    LOG_INFO("=========================================================");
}
