#include "export_manager.h"
#include "../common/logger.h"
#include "../tracker/resource_tracker.h"
#include "../hooks/hook_manager.h"
#include <cstdio>
#include <cmath>
#include <vector>

ExportManager::ExportManager() {}

ExportManager::~ExportManager() {
    ReleaseTextures();
    for (int i = 0; i < 3; i++) {
        if (m_copyAllocators[i]) m_copyAllocators[i]->Release();
    }
    if (m_copyList) m_copyList->Release();
    if (m_mvCornerReadback) {
        if (m_mvCornerMapped) { m_mvCornerReadback->Unmap(0, nullptr); m_mvCornerMapped = nullptr; }
        m_mvCornerReadback->Release(); m_mvCornerReadback = nullptr;
    }
    if (m_copyOrderFence) m_copyOrderFence->Release();
    if (m_copyQueue) m_copyQueue->Release();
    if (m_cachedSc3) m_cachedSc3->Release();
    if (m_device) m_device->Release();
}

ExportManager& ExportManager::Instance() {
    static ExportManager instance;
    return instance;
}

void ExportManager::SetDevice(ID3D12Device* device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device == device) return;
    if (m_device) m_device->Release();

    m_device = device;
    m_device->AddRef();
    LOG_INFO("ExportManager: Device updated to 0x%p", m_device);

    for (int i = 0; i < 3; i++) {
        if (m_copyAllocators[i]) { m_copyAllocators[i]->Release(); m_copyAllocators[i] = nullptr; }
    }
    if (m_copyList) { m_copyList->Release(); m_copyList = nullptr; }
    if (m_copyOrderFence) { m_copyOrderFence->Release(); m_copyOrderFence = nullptr; }
    if (m_copyQueue) { m_copyQueue->Release(); m_copyQueue = nullptr; }

    // Dedicated COPY queue: the color capture runs on the DMA engine, parallel to the game's
    // flip and next-frame rendering, rather than serialized ahead of the flip on the present
    // queue.
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    m_device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_copyQueue));

    for (int i = 0; i < 3; i++) {
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_copyAllocators[i]));
    }
    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copyAllocators[0], nullptr, IID_PPV_ARGS(&m_copyList));
    m_copyList->Close();

    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyOrderFence));
    m_copyOrderValue = 0;
}

void ExportManager::SetActiveCommandQueue(ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue == queue) return;
    m_queue = queue;
    LOG_INFO("ExportManager: Command queue updated to 0x%p (Type=%d)", m_queue, queue->GetDesc().Type);
}

void ExportManager::InitializeSwapChain(IDXGISwapChain* swapchain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    LOG_INFO("ExportManager: Swapchain initialized at 0x%p", swapchain);
}

void ExportManager::ReleaseTextures() {
    // Full GPU flush BEFORE freeing anything. The per-slot fences below only cover copies
    // WE submitted in previous frames; they do NOT cover the depth/MV CopyResource that the
    // current frame's FSR3 dispatch recorded onto the game's command list and the game just
    // submitted to this same queue. Freeing the export textures while that copy is still
    // in flight is a use-after-free → TDR/DEVICE_REMOVED. A queue-wide signal+wait drains
    // everything submitted so far, including the game's dispatch copy. Recreation is rare
    // (only when the engine reallocates its depth/MV buffers), so the stall cost is trivial.
    if (m_queue && m_device) {
        ID3D12Fence* flushFence = nullptr;
        if (SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flushFence)))) {
            if (SUCCEEDED(m_queue->Signal(flushFence, 1))) {
                if (flushFence->GetCompletedValue() < 1) {
                    HANDLE e = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                    if (e) {
                        flushFence->SetEventOnCompletion(1, e);
                        WaitForSingleObject(e, 2000);
                        CloseHandle(e);
                    }
                }
            }
            flushFence->Release();
        }
    }

    for (int i = 0; i < 3; i++) {
        auto& slot = m_slots[i];
        if (slot.fence && slot.currentFenceValue > 0 &&
            slot.fence->GetCompletedValue() < slot.currentFenceValue) {
            if (slot.fenceEvent) {
                slot.fence->SetEventOnCompletion(slot.currentFenceValue, slot.fenceEvent);
                WaitForSingleObject(slot.fenceEvent, INFINITE);
            } else {
                HANDLE e = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                if (e) {
                    slot.fence->SetEventOnCompletion(slot.currentFenceValue, e);
                    WaitForSingleObject(e, INFINITE);
                    CloseHandle(e);
                }
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        if (m_slots[i].colorTex) { m_slots[i].colorTex->Release(); m_slots[i].colorTex = nullptr; }
        if (m_slots[i].depthTex) { m_slots[i].depthTex->Release(); m_slots[i].depthTex = nullptr; }
        if (m_slots[i].mvTex) { m_slots[i].mvTex->Release(); m_slots[i].mvTex = nullptr; }
        if (m_slots[i].fgHudlessTex) { m_slots[i].fgHudlessTex->Release(); m_slots[i].fgHudlessTex = nullptr; }
        if (m_slots[i].fgUiTex) { m_slots[i].fgUiTex->Release(); m_slots[i].fgUiTex = nullptr; }
        if (m_slots[i].fence) { m_slots[i].fence->Release(); m_slots[i].fence = nullptr; }
        if (m_slots[i].fenceEvent) { CloseHandle(m_slots[i].fenceEvent); m_slots[i].fenceEvent = nullptr; }
        m_slots[i].currentFenceValue = 0;
        m_slots[i].depthValid = false;
        m_slots[i].mvValid = false;
        m_slots[i].fgHudlessValid = false;
    }

    m_texturesInitialized = false;
}

static bool AreFormatsCompatible(DXGI_FORMAT src, DXGI_FORMAT dst) {
    if (src == dst) return true;
    if ((src == DXGI_FORMAT_D32_FLOAT || src == DXGI_FORMAT_R32_FLOAT || src == DXGI_FORMAT_R32_TYPELESS) &&
        (dst == DXGI_FORMAT_D32_FLOAT || dst == DXGI_FORMAT_R32_FLOAT || dst == DXGI_FORMAT_R32_TYPELESS))
        return true;
    if ((src == DXGI_FORMAT_D16_UNORM || src == DXGI_FORMAT_R16_UNORM || src == DXGI_FORMAT_R16_TYPELESS || src == DXGI_FORMAT_R16_FLOAT) &&
        (dst == DXGI_FORMAT_D16_UNORM || dst == DXGI_FORMAT_R16_UNORM || dst == DXGI_FORMAT_R16_TYPELESS || dst == DXGI_FORMAT_R16_FLOAT))
        return true;
    if ((src == DXGI_FORMAT_D24_UNORM_S8_UINT || src == DXGI_FORMAT_R24G8_TYPELESS || src == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) &&
        (dst == DXGI_FORMAT_D24_UNORM_S8_UINT || dst == DXGI_FORMAT_R24G8_TYPELESS || dst == DXGI_FORMAT_R24_UNORM_X8_TYPELESS))
        return true;
    if ((src == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || src == DXGI_FORMAT_R32G8X24_TYPELESS || src == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
         src == DXGI_FORMAT_D32_FLOAT || src == DXGI_FORMAT_R32_FLOAT || src == DXGI_FORMAT_R32_TYPELESS) &&
        (dst == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dst == DXGI_FORMAT_R32G8X24_TYPELESS || dst == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
         dst == DXGI_FORMAT_D32_FLOAT || dst == DXGI_FORMAT_R32_FLOAT || dst == DXGI_FORMAT_R32_TYPELESS))
        return true;
    if ((src == DXGI_FORMAT_R16G16_FLOAT || src == DXGI_FORMAT_R16G16_UNORM || src == DXGI_FORMAT_R16G16_SNORM || src == DXGI_FORMAT_R16G16_TYPELESS) &&
        (dst == DXGI_FORMAT_R16G16_FLOAT || dst == DXGI_FORMAT_R16G16_UNORM || dst == DXGI_FORMAT_R16G16_SNORM || dst == DXGI_FORMAT_R16G16_TYPELESS))
        return true;
    if ((src == DXGI_FORMAT_R16G16B16A16_FLOAT || src == DXGI_FORMAT_R16G16B16A16_UNORM || src == DXGI_FORMAT_R16G16B16A16_UINT ||
         src == DXGI_FORMAT_R16G16B16A16_SNORM || src == DXGI_FORMAT_R16G16B16A16_SINT || src == DXGI_FORMAT_R16G16B16A16_TYPELESS) &&
        (dst == DXGI_FORMAT_R16G16B16A16_FLOAT || dst == DXGI_FORMAT_R16G16B16A16_UNORM || dst == DXGI_FORMAT_R16G16B16A16_UINT ||
         dst == DXGI_FORMAT_R16G16B16A16_SNORM || dst == DXGI_FORMAT_R16G16B16A16_SINT || dst == DXGI_FORMAT_R16G16B16A16_TYPELESS))
        return true;
    if ((src == DXGI_FORMAT_R8G8B8A8_UNORM || src == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || src == DXGI_FORMAT_R8G8B8A8_UINT ||
         src == DXGI_FORMAT_R8G8B8A8_SNORM || src == DXGI_FORMAT_R8G8B8A8_SINT || src == DXGI_FORMAT_R8G8B8A8_TYPELESS) &&
        (dst == DXGI_FORMAT_R8G8B8A8_UNORM || dst == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || dst == DXGI_FORMAT_R8G8B8A8_UINT ||
         dst == DXGI_FORMAT_R8G8B8A8_SNORM || dst == DXGI_FORMAT_R8G8B8A8_SINT || dst == DXGI_FORMAT_R8G8B8A8_TYPELESS))
        return true;
    return false;
}

static DXGI_FORMAT GetTypedFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:        return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:        return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:              return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:              return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   return DXGI_FORMAT_R32_FLOAT;
        default:                                 return format;
    }
}

static DXGI_FORMAT GetSafeQueryFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:       return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:      return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:             return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:             return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:  return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_FLOAT:                return DXGI_FORMAT_R16_FLOAT;
        default:                                   return format;
    }
}

bool ExportManager::RecreateExportTextures(
    uint32_t colorWidth, uint32_t colorHeight, DXGI_FORMAT colorFmt, D3D12_RESOURCE_FLAGS colorFlags,
    uint32_t depthWidth, uint32_t depthHeight, DXGI_FORMAT depthFmt, D3D12_RESOURCE_FLAGS depthFlags,
    uint32_t mvWidth, uint32_t mvHeight, DXGI_FORMAT mvFmt, D3D12_RESOURCE_FLAGS mvFlags) {

    DXGI_FORMAT exportColorFmt = colorFmt;

    DXGI_FORMAT exportDepthFmt = DXGI_FORMAT_R32_FLOAT;
    if (depthFmt == DXGI_FORMAT_D16_UNORM || depthFmt == DXGI_FORMAT_R16_TYPELESS ||
        depthFmt == DXGI_FORMAT_R16_UNORM || depthFmt == DXGI_FORMAT_R16_FLOAT) {
        exportDepthFmt = DXGI_FORMAT_R16_UNORM;
    } else if (depthFmt == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFmt == DXGI_FORMAT_R24G8_TYPELESS ||
               depthFmt == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
        exportDepthFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    } else if (depthFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || depthFmt == DXGI_FORMAT_R32G8X24_TYPELESS ||
               depthFmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
        exportDepthFmt = DXGI_FORMAT_R32_FLOAT;
    }

    DXGI_FORMAT exportMVFmt = (mvFmt != DXGI_FORMAT_UNKNOWN) ? mvFmt : DXGI_FORMAT_R16G16_FLOAT;

    // FG HUDLessColor format: use the real captured format when available, fall back
    // to the backbuffer format. This prevents the format mismatch that caused the
    // DEVICE_REMOVED crash (e.g. R16G16B16A16_FLOAT source → R8G8B8A8_UNORM dest).
    DXGI_FORMAT exportFgHudlessFmt = (m_fgHudlessFmt != DXGI_FORMAT_UNKNOWN)
                                     ? m_fgHudlessFmt : exportColorFmt;

    if (m_texturesInitialized &&
        m_colorWidth == colorWidth && m_colorHeight == colorHeight &&
        m_colorFormat == exportColorFmt && m_colorFlags == colorFlags &&
        m_depthWidth == depthWidth && m_depthHeight == depthHeight &&
        m_depthFormat == exportDepthFmt && m_depthFlags == depthFlags &&
        m_mvWidth == mvWidth && m_mvHeight == mvHeight &&
        m_mvFormat == exportMVFmt && m_mvFlags == mvFlags &&
        m_fgHudlessExportFmt == exportFgHudlessFmt) {
        return true;
    }

    LOG_INFO("Recreating export textures:\n  Color: %ux%u Fmt=%u Flags=0x%X\n  Depth: %ux%u Fmt=%u Flags=0x%X\n  MV: %ux%u Fmt=%u Flags=0x%X\n  FG Hudless: Fmt=%u",
             colorWidth, colorHeight, colorFmt, (uint32_t)colorFlags,
             depthWidth, depthHeight, depthFmt, (uint32_t)depthFlags,
             mvWidth, mvHeight, mvFmt, (uint32_t)mvFlags,
             (uint32_t)exportFgHudlessFmt);

    ReleaseTextures();

    m_colorWidth = colorWidth; m_colorHeight = colorHeight; m_colorFormat = exportColorFmt; m_colorFlags = colorFlags;
    m_depthWidth = depthWidth; m_depthHeight = depthHeight; m_depthFormat = exportDepthFmt; m_depthFlags = depthFlags;
    m_mvWidth = mvWidth;       m_mvHeight = mvHeight;       m_mvFormat = exportMVFmt;       m_mvFlags = mvFlags;
    m_fgHudlessExportFmt = exportFgHudlessFmt;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (int i = 0; i < 3; i++) {
        // Color — same format and flags as the FSR3 color source.
        // CopyResource into this texture works correctly because the game's command list
        // (where we insert the copy) already contains FSR3's DCC-decompressing barriers.
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = colorWidth;
        desc.Height           = colorHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = exportColorFmt;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // ALLOW_SIMULTANEOUS_ACCESS: the presenter thread SRV-samples these textures while the
        // copy queue may still be writing the next frame's copy. Simultaneous access permits
        // coherent concurrent read from COMMON without conflicting state transitions that would
        // otherwise fault the GPU. It is incompatible with ALLOW_DEPTH_STENCIL, so strip that bit.
        desc.Flags            = (colorFlags & ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                                | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_slots[i].colorTex));
        if (FAILED(hr)) { LOG_ERROR("Failed to create color texture for slot %d! hr=0x%X", i, hr); return false; }

        // Depth
        desc.Width  = depthWidth; desc.Height = depthHeight;
        desc.Format = exportDepthFmt;
        desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_slots[i].depthTex));
        if (FAILED(hr)) { LOG_ERROR("Failed to create depth texture for slot %d! hr=0x%X", i, hr); return false; }

        // MV
        desc.Width  = mvWidth; desc.Height = mvHeight;
        desc.Format = exportMVFmt;
        desc.Flags  = (mvFlags & ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                      | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_slots[i].mvTex));
        if (FAILED(hr)) { LOG_ERROR("Failed to create MV texture for slot %d! hr=0x%X", i, hr); return false; }

        // Fence
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_slots[i].fence));
        if (FAILED(hr)) { LOG_ERROR("Failed to create fence for slot %d! hr=0x%X", i, hr); return false; }

        m_slots[i].fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        m_slots[i].currentFenceValue = 0;

        // FG HUD-less resource copy slot — use the real HUDLessColor format, NOT
        // the backbuffer format (they differ when the engine's internal color format
        // is e.g. R10G10B10A2_UNORM while the swap chain is R8G8B8A8_UNORM).
        desc.Width = colorWidth; desc.Height = colorHeight;
        desc.Format = exportFgHudlessFmt;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_slots[i].fgHudlessTex));
        if (FAILED(hr)) { LOG_ERROR("Failed to create fgHudless texture for slot %d! hr=0x%X (fmt=%u)", i, hr, exportFgHudlessFmt); return false; }

        // FG UI resource copy slot — same format as fgHudless for consistency.
        hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_slots[i].fgUiTex));
        if (FAILED(hr)) { LOG_ERROR("Failed to create fgUi texture for slot %d! hr=0x%X", i, hr); return false; }
    }

    m_texturesInitialized = true;
    return true;
}

void ExportManager::CopyResourceToExport(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* src, ID3D12Resource* dst) {
    if (!src || !dst) return;

    auto srcDesc = src->GetDesc();
    auto dstDesc = dst->GetDesc();

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = dst;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource        = src;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_BOX box = {};
    box.right  = (UINT)((srcDesc.Width  < dstDesc.Width)  ? srcDesc.Width  : dstDesc.Width);
    box.bottom = (srcDesc.Height < dstDesc.Height) ? srcDesc.Height : dstDesc.Height;
    box.back   = 1;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
}

// SRV-sampleable view format for a (possibly typeless) depth export texture.
static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:                 return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:     return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:                 return DXGI_FORMAT_R16_UNORM;
        default:                                    return f;
    }
}

bool ExportManager::GetDebugCapture(DebugCapture& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_lastPubValid || !m_texturesInitialized) return false;

    // Promote the published slot to "debug ready" only once its color-copy fence has signaled;
    // otherwise keep showing the last complete slot. This prevents sampling a texture mid-copy.
    {
        ExportSlot& pub = m_slots[m_lastPubSlot];
        if (pub.colorTex && pub.fence &&
            pub.fence->GetCompletedValue() >= pub.currentFenceValue) {
            m_debugReadySlot  = m_lastPubSlot;
            m_debugReadyValid = true;
        }
    }
    if (!m_debugReadyValid) return false;

    ExportSlot& slot = m_slots[m_debugReadySlot];
    if (!slot.colorTex) return false;

    out.color      = slot.colorTex;
    out.colorSrvFmt = GetTypedFormat(m_colorFormat);
    out.colorW = m_colorWidth; out.colorH = m_colorHeight;

    if (slot.depthTex) {
        out.depth      = slot.depthTex;
        out.depthSrvFmt = DepthSrvFormat(slot.depthTex->GetDesc().Format);
        out.depthW = m_depthWidth; out.depthH = m_depthHeight;
    }
    if (slot.mvTex) {
        out.mv      = slot.mvTex;
        out.mvSrvFmt = GetTypedFormat(slot.mvTex->GetDesc().Format);
        out.mvW = m_mvWidth; out.mvH = m_mvHeight;
    }

    out.validity = m_lastPubValidity;
    out.seq      = m_lastPubSeq;
    out.mvScaleX = m_mvScaleX; out.mvScaleY = m_mvScaleY;
    out.camNear  = m_camNear;  out.camFar = m_camFar; out.camFovV = m_camFovV;
    out.renderW  = m_renderW;  out.renderH = m_renderH;

    // Only hand out the hud-less buffer if it is the SAME FRAME as this slot's color. When the FG
    // dispatch stalls (menus/cutscenes/loading), hudlessSeq freezes while colorSeq keeps advancing,
    // so the slot would pair a current color with a stale hud-less buffer — compositing that produces
    // full-screen garbage. Gating on the per-slot seq match makes the warp fall back to a plain
    // full-frame warp (HUD swims slightly) instead of soup whenever the pairing isn't fresh.
    bool hudlessFresh = (slot.colorSeq >= slot.hudlessSeq) && (slot.colorSeq - slot.hudlessSeq <= 1);
    if (slot.fgHudlessTex && (out.validity & 16) && hudlessFresh) {
        out.fgHudless = slot.fgHudlessTex;
        out.fgHudlessSrvFmt = GetTypedFormat(slot.fgHudlessTex->GetDesc().Format);
        out.fgHudlessW = m_colorWidth;
        out.fgHudlessH = m_colorHeight;
    }

    if (slot.fgUiTex) {
        out.fgUi = slot.fgUiTex;
        out.fgUiSrvFmt = GetTypedFormat(slot.fgUiTex->GetDesc().Format);
        out.fgUiW = m_colorWidth;
        out.fgUiH = m_colorHeight;
    }

    return true;
}

void ExportManager::SetUpscalerParams(float mvScaleX, float mvScaleY, float jitterX, float jitterY,
                                      uint32_t renderW, uint32_t renderH,
                                      float camNear, float camFar, float camFovV) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mvScaleX = mvScaleX; m_mvScaleY = mvScaleY;
    m_jitterX  = jitterX;  m_jitterY  = jitterY;
    m_renderW  = renderW;  m_renderH  = renderH;
    m_camNear  = camNear;  m_camFar   = camFar; m_camFovV = camFovV;
    m_upscalerParamsValid = true;
}

// Records CopyResource commands for depth/MV into the game's OWN FSR3 dispatch command list.
//
// This is the only correct place to touch these engine resources: the FSR3 dispatch contract
// guarantees depth and motion vectors are provided in a shader-read state
// (FFX_RESOURCE_STATE_COMPUTE_READ == D3D12 NON_PIXEL_SHADER_RESOURCE), so the barrier
// StateBefore is deterministic — no ResourceTracker guessing. And because we record onto the
// game's command list, our copy executes in correct GPU-timeline order with the engine's own
// synchronization, instead of racing it from a separately-submitted list at Present time.
void ExportManager::CopyDepthMVOnDispatch(ID3D12GraphicsCommandList* cmdList,
                                          ID3D12Resource* depth,
                                          ID3D12Resource* mv) {
    if (!cmdList || !m_device) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Device health check: detect if something already killed the device before we touch anything.
    {
        HRESULT dr = m_device->GetDeviceRemovedReason();
        if (FAILED(dr)) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                LOG_ERROR("CopyDepthMVOnDispatch: device already removed (reason=0x%X) — skipping.", dr);
                loggedOnce = true;
            }
            return;
        }
    }

    // Export textures are created/sized at Present from the sticky FSR3 parameter cache.
    // On the very first frame they may not exist yet — depth/MV are then captured starting
    // the next frame. We never create textures here (that would race the Present thread).
    if (!m_texturesInitialized) {
        static int uninitCount = 0;
        if (++uninitCount <= 3) {
            LOG_INFO("CopyDepthMVOnDispatch: textures not initialized yet (call #%d)", uninitCount);
        }
        return;
    }

    uint32_t slotIndex = m_sequenceNumber % 3;
    auto& slot = m_slots[slotIndex];

    // FSR3 input contract: depth/MV are provided as shader-read resources.
    constexpr D3D12_RESOURCE_STATES kFsrInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Helper: transition src(kFsrInputState)→COPY_SOURCE + dst(COMMON)→COPY_DEST, copy, restore.
    auto copyOne = [&](ID3D12Resource* src, ID3D12Resource* dst) {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource   = src;
        b[0].Transition.StateBefore = kFsrInputState;
        b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource   = dst;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(2, b);

        CopyResourceToExport(cmdList, src, dst);

        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter  = kFsrInputState;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        cmdList->ResourceBarrier(2, b);
    };

    bool depthCopied = false, mvCopied = false;
    if (depth && slot.depthTex) {
        auto sd = depth->GetDesc();
        auto dd = slot.depthTex->GetDesc();
        // Dimension + format gate: if the export slot doesn't match the source, skip.
        // RecreateExportTextures (at Present) will resize, and the next dispatch succeeds.
        // Without this check, the copy records references to export textures that may be
        // freed by RecreateExportTextures before the GPU executes the game's command list,
        // causing a use-after-free → TDR / DEVICE_REMOVED.
        if (AreFormatsCompatible(sd.Format, dd.Format) &&
            (uint32_t)sd.Width == (uint32_t)dd.Width && sd.Height == dd.Height) {
            copyOne(depth, slot.depthTex);
            depthCopied = true;
            slot.depthValid = true;
        } else {
            static int skipCount = 0;
            if (++skipCount <= 5) {
                LOG_WARN("CopyDepthMV: depth SKIPPED — src=%ux%u fmt=%u dst=%ux%u fmt=%u compat=%d",
                         (uint32_t)sd.Width, sd.Height, sd.Format,
                         (uint32_t)dd.Width, dd.Height, dd.Format,
                         AreFormatsCompatible(sd.Format, dd.Format));
            }
        }
    } else {
        static int nullCount = 0;
        if (++nullCount <= 3) {
            LOG_WARN("CopyDepthMV: depth null check failed — depth=%p depthTex=%p", depth, slot.depthTex);
        }
    }
    if (mv && slot.mvTex) {
        auto sm = mv->GetDesc();
        auto dm = slot.mvTex->GetDesc();
        if (AreFormatsCompatible(sm.Format, dm.Format) &&
            (uint32_t)sm.Width == (uint32_t)dm.Width && sm.Height == dm.Height) {
            copyOne(mv, slot.mvTex);
            mvCopied = true;
            slot.mvValid = true;

            // Gain auto-calibration: DMA the 4 far-corner MV texels (pure camera screen motion) into a
            // CPU-readable buffer for WarpRenderer's mouse<->MV regression. Reads our own slot.mvTex copy
            // (now in COMMON), so it never touches an engine resource. Cyberpunk stores MV in UV space
            // (motionVectorScale == render res), so these are small fractions — ReadCornerMV scales them.
            auto mvDesc = slot.mvTex->GetDesc();
            uint32_t mvw = (uint32_t)mvDesc.Width, mvh = mvDesc.Height;
            if (mvw >= 8 && mvh >= 8) {
                if (!m_mvCornerReadback) {
                    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC rd = {};
                    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width            = 2048;   // 4 corners x 512-byte-aligned placed footprints
                    rd.Height           = 1;
                    rd.DepthOrArraySize = 1;
                    rd.MipLevels        = 1;
                    rd.Format           = DXGI_FORMAT_UNKNOWN;
                    rd.SampleDesc.Count = 1;
                    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if (SUCCEEDED(m_device->CreateCommittedResource(
                            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                            nullptr, IID_PPV_ARGS(&m_mvCornerReadback)))) {
                        D3D12_RANGE noRead = { 0, 0 };
                        m_mvCornerReadback->Map(0, &noRead, &m_mvCornerMapped);
                    }
                }
                if (m_mvCornerReadback && m_mvCornerMapped) {
                    D3D12_RESOURCE_BARRIER tb = {};
                    tb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    tb.Transition.pResource   = slot.mvTex;
                    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cmdList->ResourceBarrier(1, &tb);

                    uint32_t insetX = mvw / 25, insetY = mvh / 25;  // ~4% inset
                    uint32_t cx[4] = { insetX, mvw - 1 - insetX, insetX,           mvw - 1 - insetX };
                    uint32_t cy[4] = { insetY, insetY,           mvh - 1 - insetY, mvh - 1 - insetY };
                    for (int c = 0; c < 4; ++c) {
                        D3D12_TEXTURE_COPY_LOCATION dstL = {};
                        dstL.pResource              = m_mvCornerReadback;
                        dstL.Type                   = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dstL.PlacedFootprint.Offset = (UINT64)c * 512;
                        dstL.PlacedFootprint.Footprint.Format   = mvDesc.Format;
                        dstL.PlacedFootprint.Footprint.Width    = 1;
                        dstL.PlacedFootprint.Footprint.Height   = 1;
                        dstL.PlacedFootprint.Footprint.Depth    = 1;
                        dstL.PlacedFootprint.Footprint.RowPitch = 256;
                        D3D12_TEXTURE_COPY_LOCATION srcL = {};
                        srcL.pResource        = slot.mvTex;
                        srcL.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        srcL.SubresourceIndex = 0;
                        D3D12_BOX box = { cx[c], cy[c], 0, cx[c] + 1, cy[c] + 1, 1 };
                        cmdList->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, &box);
                    }

                    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
                    cmdList->ResourceBarrier(1, &tb);

                    // Cyberpunk's MV is UV-space (motionVectorScale == render res), so the raw stored value
                    // IS the per-frame UV screen shift the gain regression wants — use scale 1.0. (If the
                    // measured gain comes out ~1280x off, the stored MV is pixel-space and this needs 1/w.)
                    m_calMvScaleX   = 1.0f;
                    m_calMvScaleY   = 1.0f;
                    m_mvCornerValid = true;
                }
            }
        }
    }

    {
        static int logCount = 0;
        if (++logCount <= 10) {
            LOG_INFO("CopyDepthMVOnDispatch: slot=%u depth=%s mv=%s (cmdList=%p)",
                     slotIndex, depthCopied ? "OK" : "SKIP", mvCopied ? "OK" : "SKIP", cmdList);
        }
    }

    m_dispatchDepthValid = depthCopied;
    m_dispatchMVValid    = mvCopied;
    m_dispatchSlot       = slotIndex;
    m_dispatchOccurred   = true;
}

// Map an ffx-api FfxApiResourceState to a D3D12 resource state. The HUDLessColor state arrives via
// the ffxConfigure path as THIS enum (bit-flags: 1=COMMON, 2=UAV, 4=COMPUTE_READ, 8=PIXEL_READ,
// 12=PIXEL_COMPUTE_READ, 16=COPY_SRC, 20=GENERIC_READ, 32=COPY_DEST, 128=PRESENT, 256=RENDER_TARGET)
// — NOT the legacy FfxResourceStates enum, which shares the names but uses different values. Cyberpunk
// reports 4 (COMPUTE_READ); the old mapper fell through to COMMON, giving the copy a wrong barrier
// StateBefore.
static D3D12_RESOURCE_STATES FfxApiStateToD3D12(uint32_t ffxApiState) {
    switch (ffxApiState) {
        case 1:   return D3D12_RESOURCE_STATE_COMMON;                       // COMMON
        case 2:   return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;             // UNORDERED_ACCESS
        case 4:   return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;    // COMPUTE_READ
        case 8:   return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;        // PIXEL_READ
        case 12:  return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // PIXEL_COMPUTE_READ
        case 16:  return D3D12_RESOURCE_STATE_COPY_SOURCE;                  // COPY_SRC
        case 20:  return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE; // GENERIC_READ
        case 32:  return D3D12_RESOURCE_STATE_COPY_DEST;                    // COPY_DEST
        case 128: return D3D12_RESOURCE_STATE_PRESENT;                      // PRESENT (== COMMON)
        case 256: return D3D12_RESOURCE_STATE_RENDER_TARGET;                // RENDER_TARGET
        default:  return D3D12_RESOURCE_STATE_COMMON;
    }
}

void ExportManager::CopyFgHudlessOnDispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* fgHudless, uint32_t ffxState) {
    if (!cmdList || !m_device || !fgHudless) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_texturesInitialized) return;

    uint32_t slotIndex = m_sequenceNumber % 3;
    auto& slot = m_slots[slotIndex];
    if (!slot.fgHudlessTex) return;

    // Cache the HUDLessColor resource format so RecreateExportTextures can use it.
    // If it differs from the current fgHudlessTex format, skip this frame's copy —
    // OnPresent will trigger a recreation with the correct format, and the next
    // dispatch will succeed.
    auto srcDesc = fgHudless->GetDesc();
    m_fgHudlessFmt   = srcDesc.Format;
    m_fgHudlessWidth = (uint32_t)srcDesc.Width;
    m_fgHudlessHeight = srcDesc.Height;

    // Format + dimension gate — prevents DEVICE_REMOVED from CopyTextureRegion
    // with mismatched bit-widths (e.g. R16G16B16A16_FLOAT → R8G8B8A8_UNORM)
    // AND prevents use-after-free when RecreateExportTextures resizes.
    auto dstDesc = slot.fgHudlessTex->GetDesc();
    if (!AreFormatsCompatible(srcDesc.Format, dstDesc.Format) ||
        (uint32_t)srcDesc.Width != (uint32_t)dstDesc.Width ||
        srcDesc.Height != dstDesc.Height) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            LOG_WARN("CopyFgHudlessOnDispatch: mismatch src=%ux%u fmt=%u dst=%ux%u fmt=%u — skipping.",
                     (uint32_t)srcDesc.Width, srcDesc.Height, srcDesc.Format,
                     (uint32_t)dstDesc.Width, dstDesc.Height, dstDesc.Format);
            loggedOnce = true;
        }
        return;
    }

    // Map the ffx-api state to D3D12 state (see FfxApiStateToD3D12 — the ffxConfigure path reports
    // the ffx-api enum, e.g. 4 = COMPUTE_READ).
    D3D12_RESOURCE_STATES kFsrInputState = FfxApiStateToD3D12(ffxState);
    static bool loggedState = false;
    if (!loggedState) {
        LOG_INFO("CopyFgHudlessOnDispatch: mapped ffx-api state %u to D3D12 state 0x%X", ffxState, (uint32_t)kFsrInputState);
        loggedState = true;
    }

    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = fgHudless;
    b[0].Transition.StateBefore = kFsrInputState;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    b[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource   = slot.fgHudlessTex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(2, b);

    CopyResourceToExport(cmdList, fgHudless, slot.fgHudlessTex);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = kFsrInputState;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;

    cmdList->ResourceBarrier(2, b);

    m_dispatchFgHudlessValid = true;
    m_dispatchSlot           = slotIndex;
    m_dispatchOccurred       = true;
    slot.fgHudlessValid      = true;
    slot.hudlessSeq          = m_sequenceNumber;   // diagnostic: pairing vs color
}


// IEEE 754 half (R16_FLOAT) -> float. Handles normals, subnormals, inf/nan.
static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {
            // subnormal: normalize
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000 | (mant << 13); // inf / nan
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &bits, sizeof(f));
    return f;
}

bool ExportManager::ReadCornerMV(float& uvU, float& uvV) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_mvCornerValid || !m_mvCornerMapped) return false;

    const uint8_t* base = (const uint8_t*)m_mvCornerMapped;
    float sumU = 0.0f, sumV = 0.0f;
    for (int c = 0; c < 4; ++c) {
        const uint16_t* px = (const uint16_t*)(base + (size_t)c * 512);
        sumU += HalfToFloat(px[0]);
        sumV += HalfToFloat(px[1]);
    }
    float mvU = (sumU * 0.25f) * m_calMvScaleX;
    float mvV = (sumV * 0.25f) * m_calMvScaleY;

    // Reject garbage / non-finite / out-of-range (a full-screen UV shift > 1 is not camera motion).
    if (!(mvU == mvU) || !(mvV == mvV)) return false;       // NaN
    if (fabsf(mvU) >= 1.0f || fabsf(mvV) >= 1.0f) return false;
    uvU = mvU; uvV = mvV;
    return true;
}

// ---- Stage 0 diagnostics ---------------------------------------------------------------------
// Confirms, from a live run, the two facts the HUD-compositor rewrite hinges on:
//   1. UI-handling mode — does the engine hand FFX a clean UI texture (trivial composite) or only
//      a HUDLessColor surface (difference-extraction required)?
//   2. Color encoding — is the warp's scene source (HUDLess) in the SAME encoding as the present
//      buffer it's differenced against? A linear-HDR-vs-display mismatch alone makes per-pixel
//      differencing invalid regardless of any other tuning.
static const char* ColorEncodingName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:  return "HDR-linear(fp32)";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "HDR-linear(fp16/scRGB)";
        case DXGI_FORMAT_R11G11B10_FLOAT:     return "HDR-linear(fp11)";
        case DXGI_FORMAT_R10G10B10A2_UNORM:   return "HDR10/display(rgb10a2)";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:      return "SDR/display(unorm8)";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "SDR/display(srgb8)";
        default:                              return "other";
    }
}
static bool IsLinearFloatEncoding(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R11G11B10_FLOAT;
}
static bool SafeGetResourceDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC& out) {
    if (!r || ((uintptr_t)r & 7) != 0) return false;
    __try { out = r->GetDesc(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void LogStage0Diagnostics(uint32_t bbW, uint32_t bbH, DXGI_FORMAT bbFmt,
                                 ID3D12Resource* hudless, DXGI_FORMAT hudlessFmt,
                                 uint32_t hudlessW, uint32_t hudlessH, uint32_t hudlessFfxState,
                                 ID3D12Resource* fgUi) {
    // Throttle: re-log only when the picture changes (presence/format), so the block is easy to find
    // and doesn't flood. Signature folds the facts we report.
    uint64_t sig = (uint64_t)(bbFmt) | ((uint64_t)hudlessFmt << 12) | ((uint64_t)hudlessFfxState << 24)
                 | ((uint64_t)(hudless ? 1 : 0) << 32) | ((uint64_t)(fgUi ? 1 : 0) << 33);
    static uint64_t lastSig = ~0ull;
    if (sig == lastSig) return;
    lastSig = sig;

    const char* uiMode = fgUi      ? "UI-TEXTURE (clean UI registered — difference-extraction NOT needed)"
                       : hudless    ? "HUDLESS (no UI texture; difference-extraction required)"
                                    : "NONE yet (no HUDLess or UI resource intercepted)";

    LOG_INFO("==== STAGE0 DIAGNOSTICS ====");
    LOG_INFO("STAGE0  UI mode: %s", uiMode);
    LOG_INFO("STAGE0  present/backbuffer: %ux%u  fmt=%u (%s)", bbW, bbH, (unsigned)bbFmt, ColorEncodingName(bbFmt));

    if (hudless) {
        LOG_INFO("STAGE0  HUDLess (warp scene source): ptr=0x%p %ux%u fmt=%u (%s) ffxState=%u",
                 hudless, hudlessW, hudlessH, (unsigned)hudlessFmt, ColorEncodingName(hudlessFmt), hudlessFfxState);
        bool bbLinear  = IsLinearFloatEncoding(bbFmt);
        bool hudLinear = IsLinearFloatEncoding(hudlessFmt);
        if (hudlessFmt == DXGI_FORMAT_UNKNOWN) {
            LOG_INFO("STAGE0  COLOR ENCODING: hudless format not cached yet (no copy executed) — re-check next frames");
        } else if (bbLinear != hudLinear) {
            LOG_WARN("STAGE0  COLOR ENCODING: *** MISMATCH *** scene=%s vs present=%s — per-pixel differencing is INVALID until matched (linearize/encode one side before compare)",
                     ColorEncodingName(hudlessFmt), ColorEncodingName(bbFmt));
        } else {
            LOG_INFO("STAGE0  COLOR ENCODING: compatible (scene=%s, present=%s) — differencing is meaningful; residual delta is post-process/grain",
                     ColorEncodingName(hudlessFmt), ColorEncodingName(bbFmt));
        }
        if (hudlessW && (hudlessW != bbW || hudlessH != bbH))
            LOG_WARN("STAGE0  RESOLUTION: HUDLess %ux%u != present %ux%u — scene source is not display-res (rescale/align needed)",
                     hudlessW, hudlessH, bbW, bbH);
    }

    if (fgUi) {
        D3D12_RESOURCE_DESC ud = {};
        if (SafeGetResourceDesc(fgUi, ud))
            LOG_INFO("STAGE0  UI texture: ptr=0x%p %ux%u fmt=%u (%s) — if alpha-bearing, composite directly (Stage-0 short-circuit)",
                     fgUi, (uint32_t)ud.Width, ud.Height, (unsigned)ud.Format, ColorEncodingName(ud.Format));
        else
            LOG_INFO("STAGE0  UI texture: ptr=0x%p (desc unreadable)", fgUi);
    }
    LOG_INFO("============================");
}

bool ExportManager::OnPresent(IDXGISwapChain* swapchain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_device || !m_queue) return false;

    m_frameId++;

    // Cache the IDXGISwapChain3 QI across presents; re-acquire only if the swapchain object
    // changes. (GetBuffer stays per-frame — it's cheap and remains correct across
    // ResizeBuffers, which we don't hook.)
    if (swapchain != m_cachedSwapchainRaw) {
        if (m_cachedSc3) { m_cachedSc3->Release(); m_cachedSc3 = nullptr; }
        swapchain->QueryInterface(IID_PPV_ARGS(&m_cachedSc3));
        m_cachedSwapchainRaw = swapchain;
    }
    UINT backbufferIdx = m_cachedSc3 ? m_cachedSc3->GetCurrentBackBufferIndex() : 0;

    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(backbufferIdx, IID_PPV_ARGS(&backbuffer)))) return false;

    auto bbDesc = backbuffer->GetDesc();
    uint32_t width = (uint32_t)bbDesc.Width, height = bbDesc.Height;

    // Stage 0: report the UI-handling mode and scene-vs-present color encoding (throttled, on-change).
    LogStage0Diagnostics(width, height, bbDesc.Format,
                         HookManager::Instance().GetInterceptedFgHudless(),
                         m_fgHudlessFmt, m_fgHudlessWidth, m_fgHudlessHeight,
                         HookManager::Instance().GetInterceptedFgHudlessState(),
                         HookManager::Instance().GetInterceptedFgUi());

    constexpr uint32_t kMaxStalePresentsBeforeFallback = 6;
    bool dispatchedThisFrame = HookManager::Instance().ConsumeDispatchFlag();
    if (dispatchedThisFrame) {
        m_presentsSinceDispatch = 0;
    } else if (++m_presentsSinceDispatch > kMaxStalePresentsBeforeFallback) {
        HookManager::Instance().ClearInterceptedResources();
    }

    ID3D12Resource* colorRes = HookManager::Instance().GetInterceptedColor();
    ID3D12Resource* depthRes = HookManager::Instance().GetInterceptedDepth();
    ID3D12Resource* mvRes    = HookManager::Instance().GetInterceptedMV();

    // When FSR3 resources are live, update the sticky format/size cache.
    // OnResourceDestroyed (via SetPrivateDataInterface) already nulls m_interceptedDepth/MV
    // when the resources are COM-destroyed, so arriving here with a non-null pointer means
    // the resource is alive — no IsResourceTracked guard needed (that guard was causing
    // RecreateExportTextures to oscillate between real and default formats every time FSR3
    // turned on/off, triggering GPU fence stalls each frame).
    if (depthRes) {
        auto d = depthRes->GetDesc();
        m_fsr3DepthFmt    = d.Format;
        m_fsr3DepthFlags  = d.Flags;
        m_fsr3DepthWidth  = (uint32_t)d.Width;
        m_fsr3DepthHeight = d.Height;
    }
    if (mvRes) {
        auto m = mvRes->GetDesc();
        m_fsr3MVFmt    = m.Format;
        m_fsr3MVFlags  = m.Flags;
        m_fsr3MVWidth  = (uint32_t)m.Width;
        m_fsr3MVHeight = m.Height;
    }

    // Don't create export textures or do any GPU work until the first FSR dispatch has
    // revealed the real render resolution. Creating textures at the backbuffer resolution
    // first, then recreating at the render resolution, requires a destructive
    // ReleaseTextures cycle that races the game's command list submissions and causes
    // DEVICE_REMOVED. By deferring, the first RecreateExportTextures creates at the
    // correct dimensions and no recreation is ever needed during boot.
    if (m_fsr3DepthWidth == 0 || m_fsr3MVWidth == 0) {
        backbuffer->Release();
        return false;
    }
    bool isUpscalerSource = (depthRes != nullptr || mvRes != nullptr);

    if (isUpscalerSource &&
        (depthRes == backbuffer || mvRes == backbuffer || colorRes == backbuffer)) {
        LOG_WARN("Sanity check: intercepted resource matches backbuffer — discarding.");
        colorRes = nullptr; depthRes = nullptr; mvRes = nullptr; isUpscalerSource = false;
    }

    if (isUpscalerSource && !dispatchedThisFrame) {
        backbuffer->Release();
        return false;
    }

    // Color always comes from the swapchain backbuffer — full resolution, all post-effects,
    // sharpening, color grading, and HUD included. Backbuffer surfaces are not DCC-compressed
    // so CopyTextureRegion works correctly without dispatch-time injection.
    uint32_t colorWidth  = width;
    uint32_t colorHeight = height;

    // Use sticky FSR3 parameters when depth/MV are temporarily absent (FSR3 inactive).
    // Once we have seen real FSR3 resources, their format/size never changes (same render
    // resolution), so RecreateExportTextures sees identical parameters on every call and
    // the early-out path is always taken — no GPU fence stall, no texture recreation.
    uint32_t depthWidth  = (m_fsr3DepthWidth  > 0) ? m_fsr3DepthWidth  : width;
    uint32_t depthHeight = (m_fsr3DepthHeight > 0) ? m_fsr3DepthHeight : height;
    uint32_t mvWidth     = (m_fsr3MVWidth     > 0) ? m_fsr3MVWidth     : width;
    uint32_t mvHeight    = (m_fsr3MVHeight    > 0) ? m_fsr3MVHeight    : height;

    DXGI_FORMAT colorFmt = bbDesc.Format;
    DXGI_FORMAT depthFmt = m_fsr3DepthFmt;
    DXGI_FORMAT mvFmt    = m_fsr3MVFmt;

    D3D12_RESOURCE_FLAGS colorFlags = bbDesc.Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_RESOURCE_FLAGS depthFlags = m_fsr3DepthFlags;
    D3D12_RESOURCE_FLAGS mvFlags    = m_fsr3MVFlags;

    // Device health check: if something (our code, the game, or the driver) already
    // removed the device, skip all GPU work — attempting to create resources or submit
    // commands on a removed device just produces cascading errors.
    {
        HRESULT dr = m_device->GetDeviceRemovedReason();
        if (FAILED(dr)) {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                LOG_ERROR("OnPresent: device already removed (reason=0x%X) — skipping all GPU work.", dr);
                loggedOnce = true;
            }
            backbuffer->Release();
            return false;
        }
    }

    if (!RecreateExportTextures(colorWidth, colorHeight, colorFmt, colorFlags,
                                depthWidth, depthHeight, depthFmt, depthFlags,
                                mvWidth, mvHeight, mvFmt, mvFlags)) {
        backbuffer->Release();
        return false;
    }

    uint32_t slotIndex = m_sequenceNumber % 3;
    auto& slot = m_slots[slotIndex];

    // Non-blocking slot-reuse check. The previous color copy into this slot — and the copy
    // allocator we are about to reset — was submitted ~3 presents ago and is virtually always
    // complete. If the GPU has fallen behind and it is not, SKIP publishing this frame rather
    // than blocking the game's present thread. Dropping a captured frame is invisible to the
    // presenter (it keeps re-warping the freshest slot it already holds); a wait on the present
    // thread is felt as a hitch. We never INFINITE-wait on the game's thread.
    if (slot.fence && slot.currentFenceValue > 0 &&
        slot.fence->GetCompletedValue() < slot.currentFenceValue) {
        backbuffer->Release();
        return false;
    }

    uint32_t validityFlags = isUpscalerSource ? 8u : 0u;

    // Use slot-persistent validity flags to handle interpolated frames correctly
    // (since dispatch copies only run on real frames, but slots remain valid on interpolated frames).
    if (slot.depthValid) validityFlags |= 2;
    if (slot.mvValid)    validityFlags |= 4;
    if (slot.fgHudlessValid) validityFlags |= 16;

    m_dispatchOccurred   = false;
    m_dispatchDepthValid = false;
    m_dispatchMVValid    = false;
    m_dispatchFgHudlessValid = false;


    // Record the color copy (backbuffer → slot color tex) on the COPY-queue list. No resource
    // barriers are needed or wanted: at Present the backbuffer is in PRESENT state, which is
    // identically COMMON (both are value 0), and the export texture sits in COMMON with
    // ALLOW_SIMULTANEOUS_ACCESS. A copy queue implicitly promotes both to COPY_SOURCE /
    // COPY_DEST for the copy and decays them back to COMMON afterward, so we never issue a
    // state transition on the swapchain buffer that the concurrent flip also reads.
    m_copyAllocators[slotIndex]->Reset();
    m_copyList->Reset(m_copyAllocators[slotIndex], nullptr);
    CopyResourceToExport(m_copyList, backbuffer, slot.colorTex);
    m_copyList->Close();

    // Order the copy AFTER the game's frame rendering (already submitted to the present queue)
    // WITHOUT delaying the flip. A queue Signal is not GPU work and does not block the present
    // queue, so the game's flip proceeds immediately; the copy then runs on the DMA engine in
    // parallel with the flip and the next frame. This signal also transitively covers the
    // depth/MV copies the dispatch recorded onto the present queue earlier this frame, so the
    // single slot.fence the presenter waits on still gates color, depth, and MV together.
    m_copyOrderValue++;
    m_queue->Signal(m_copyOrderFence, m_copyOrderValue);
    m_copyQueue->Wait(m_copyOrderFence, m_copyOrderValue);
    m_copyQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&m_copyList);

    slot.currentFenceValue++;
    m_copyQueue->Signal(slot.fence, slot.currentFenceValue);
    validityFlags |= 1;
    slot.colorSeq = m_sequenceNumber;   // diagnostic: pairing vs hudless

    // Frame-skew probe: color (here) and hudless (FG dispatch) should share m_sequenceNumber if they
    // are the same frame. A nonzero delta = bookkeeping skew; delta 0 while the debug mask still shows
    // moving edge outlines = content skew (HUDLessColor lags the backbuffer). Throttled.
    {
        static uint64_t lastLog = 0;
        if (slot.fgHudlessValid && (m_sequenceNumber < 5 || m_sequenceNumber - lastLog >= 120)) {
            lastLog = m_sequenceNumber;
            LOG_INFO("PAIR: slot=%u colorSeq=%llu hudlessSeq=%llu delta=%lld",
                     slotIndex, (unsigned long long)slot.colorSeq,
                     (unsigned long long)slot.hudlessSeq,
                     (long long)slot.colorSeq - (long long)slot.hudlessSeq);
        }
    }

    if (m_upscalerParamsValid) validityFlags |= 0x8;  // bit 3: upscaler reprojection params valid

    // DEFERRED PUBLISH: promote the PREVIOUS frame's slot to the in-process debug views now. Its
    // GPU copies — including the depth/MV copy that rides the game's FSR dispatch list on a
    // graphics/async-compute queue we can't fence directly — are guaranteed complete a full frame
    // later. Promoting immediately would let the presenter sample a slot whose depth/MV copy was
    // still in flight (the torn-depth race). Cost: +1 frame of content age, ~0 GPU/CPU overhead.
    if (m_pendingValid) {
        m_lastPubSlot     = m_pendingSlot;
        m_lastPubValidity = m_pendingValidity;
        m_lastPubSeq      = m_pendingSeq;
        m_lastPubValid    = true;
    }
    m_pendingValidity = validityFlags;
    m_pendingSeq      = m_sequenceNumber;
    m_pendingSlot     = slotIndex;
    m_pendingValid    = true;

    m_sequenceNumber++;

    backbuffer->Release();
    return true;
}
