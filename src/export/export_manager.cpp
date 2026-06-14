#include "export_manager.h"
#include "../common/logger.h"
#include "../tracker/resource_tracker.h"
#include <cstdio>

ExportManager::ExportManager() {
    InitializeIPC();
}

ExportManager::~ExportManager() {
    ReleaseTextures();
    if (m_sharedMetadata) {
        UnmapViewOfFile(m_sharedMetadata);
    }
    if (m_fileMapping) {
        CloseHandle(m_fileMapping);
    }
    for (int i = 0; i < 3; i++) {
        if (m_cmdAllocators[i]) m_cmdAllocators[i]->Release();
    }
    if (m_cmdList) m_cmdList->Release();
    if (m_device) m_device->Release();
}

ExportManager& ExportManager::Instance() {
    static ExportManager instance;
    return instance;
}

bool ExportManager::InitializeIPC() {
    if (m_ipcInitialized) return true;

    // Create named shared memory block
    m_fileMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SharedRingBuffer),
        L"Local\\AsyncReproj_SharedMemory"
    );

    if (!m_fileMapping) {
        LOG_ERROR("Failed to create shared memory mapping!");
        return false;
    }

    m_sharedMetadata = (SharedRingBuffer*)MapViewOfFile(
        m_fileMapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        sizeof(SharedRingBuffer)
    );

    if (!m_sharedMetadata) {
        LOG_ERROR("Failed to map shared memory view!");
        CloseHandle(m_fileMapping);
        m_fileMapping = NULL;
        return false;
    }

    // Initialize shared metadata header
    m_sharedMetadata->magic = 0x52455052; // "REPR"
    m_sharedMetadata->producerPid = GetCurrentProcessId();
    m_sharedMetadata->activeSlotIndex = 0;
    memset(m_sharedMetadata->slots, 0, sizeof(m_sharedMetadata->slots));

    m_ipcInitialized = true;
    LOG_INFO("IPC shared memory initialized successfully.");
    return true;
}

void ExportManager::SetDevice(ID3D12Device* device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_device == device) return;
    if (m_device) m_device->Release();
    
    m_device = device;
    m_device->AddRef();
    LOG_INFO("ExportManager: Device updated to 0x%p", m_device);
    
    // Release old command allocators/list if any
    for (int i = 0; i < 3; i++) {
        if (m_cmdAllocators[i]) {
            m_cmdAllocators[i]->Release();
            m_cmdAllocators[i] = nullptr;
        }
    }
    if (m_cmdList) {
        m_cmdList->Release();
        m_cmdList = nullptr;
    }

    // Create command allocators and command list
    for (int i = 0; i < 3; i++) {
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i]));
    }
    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocators[0], nullptr, IID_PPV_ARGS(&m_cmdList));
    m_cmdList->Close();
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
    // Wait for GPU to finish on all slots before destroying resources
    for (int i = 0; i < 3; i++) {
        auto& slot = m_slots[i];
        if (slot.fence && slot.currentFenceValue > 0) {
            if (slot.fence->GetCompletedValue() < slot.currentFenceValue) {
                if (slot.fenceEvent) {
                    slot.fence->SetEventOnCompletion(slot.currentFenceValue, slot.fenceEvent);
                    WaitForSingleObject(slot.fenceEvent, INFINITE);
                } else {
                    HANDLE hEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                    if (hEvent) {
                        slot.fence->SetEventOnCompletion(slot.currentFenceValue, hEvent);
                        WaitForSingleObject(hEvent, INFINITE);
                        CloseHandle(hEvent);
                    }
                }
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        if (m_slots[i].colorTex) { m_slots[i].colorTex->Release(); m_slots[i].colorTex = nullptr; }
        if (m_slots[i].depthTex) { m_slots[i].depthTex->Release(); m_slots[i].depthTex = nullptr; }
        if (m_slots[i].mvTex) { m_slots[i].mvTex->Release(); m_slots[i].mvTex = nullptr; }
        if (m_slots[i].fence) { m_slots[i].fence->Release(); m_slots[i].fence = nullptr; }
        if (m_slots[i].fenceEvent) { CloseHandle(m_slots[i].fenceEvent); m_slots[i].fenceEvent = nullptr; }
        m_slots[i].currentFenceValue = 0;
    }
    m_texturesInitialized = false;
}

bool AreFormatsCompatible(DXGI_FORMAT src, DXGI_FORMAT dst) {
    if (src == dst) return true;

    // 32-bit float family
    if ((src == DXGI_FORMAT_D32_FLOAT || src == DXGI_FORMAT_R32_FLOAT || src == DXGI_FORMAT_R32_TYPELESS) &&
        (dst == DXGI_FORMAT_D32_FLOAT || dst == DXGI_FORMAT_R32_FLOAT || dst == DXGI_FORMAT_R32_TYPELESS)) {
        return true;
    }

    // 16-bit family
    if ((src == DXGI_FORMAT_D16_UNORM || src == DXGI_FORMAT_R16_UNORM || src == DXGI_FORMAT_R16_TYPELESS || src == DXGI_FORMAT_R16_FLOAT) &&
        (dst == DXGI_FORMAT_D16_UNORM || dst == DXGI_FORMAT_R16_UNORM || dst == DXGI_FORMAT_R16_TYPELESS || dst == DXGI_FORMAT_R16_FLOAT)) {
        return true;
    }

    // 24-bit/8-bit family
    if ((src == DXGI_FORMAT_D24_UNORM_S8_UINT || src == DXGI_FORMAT_R24G8_TYPELESS || src == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) &&
        (dst == DXGI_FORMAT_D24_UNORM_S8_UINT || dst == DXGI_FORMAT_R24G8_TYPELESS || dst == DXGI_FORMAT_R24_UNORM_X8_TYPELESS)) {
        return true;
    }

    // 64-bit family
    if ((src == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || src == DXGI_FORMAT_R32G8X24_TYPELESS || src == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) &&
        (dst == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dst == DXGI_FORMAT_R32G8X24_TYPELESS || dst == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)) {
        return true;
    }

    // R16G16 family
    if ((src == DXGI_FORMAT_R16G16_FLOAT || src == DXGI_FORMAT_R16G16_UNORM || src == DXGI_FORMAT_R16G16_SNORM || src == DXGI_FORMAT_R16G16_TYPELESS) &&
        (dst == DXGI_FORMAT_R16G16_FLOAT || dst == DXGI_FORMAT_R16G16_UNORM || dst == DXGI_FORMAT_R16G16_SNORM || dst == DXGI_FORMAT_R16G16_TYPELESS)) {
        return true;
    }

    return false;
}

bool ExportManager::RecreateExportTextures(uint32_t width, uint32_t height, DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvFmt) {
    if (m_texturesInitialized && m_width == width && m_height == height && m_colorFormat == colorFmt && m_depthFormat == depthFmt && m_mvFormat == mvFmt) {
        return true;
    }

    LOG_INFO("Recreating export textures: Width=%u, Height=%u, ColorFmt=%u, DepthFmt=%u, MVFmt=%u", width, height, colorFmt, depthFmt, mvFmt);
    ReleaseTextures();

    m_width = width;
    m_height = height;
    m_colorFormat = colorFmt;
    m_depthFormat = depthFmt;
    m_mvFormat = mvFmt;

    for (int i = 0; i < 3; i++) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = colorFmt;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Create Shared Color
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_SHARED,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_slots[i].colorTex)
        );
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared color texture for slot %d! hr = 0x%X", i, hr);
            return false;
        }

        wchar_t name[128];
        swprintf_s(name, L"Local\\AsyncReproj_Color_%d", i);
        HANDLE hShared = nullptr;
        m_device->CreateSharedHandle(m_slots[i].colorTex, nullptr, GENERIC_ALL, name, &hShared);
        if (hShared) CloseHandle(hShared);

        // Create Shared Depth (expose as typeless family format)
        DXGI_FORMAT exportDepthFmt = DXGI_FORMAT_R32_TYPELESS;
        if (depthFmt == DXGI_FORMAT_D16_UNORM || depthFmt == DXGI_FORMAT_R16_TYPELESS || depthFmt == DXGI_FORMAT_R16_UNORM || depthFmt == DXGI_FORMAT_R16_FLOAT) {
            exportDepthFmt = DXGI_FORMAT_R16_TYPELESS;
        } else if (depthFmt == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFmt == DXGI_FORMAT_R24G8_TYPELESS || depthFmt == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
            exportDepthFmt = DXGI_FORMAT_R24G8_TYPELESS;
        } else if (depthFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || depthFmt == DXGI_FORMAT_R32G8X24_TYPELESS || depthFmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
            exportDepthFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
        }
        
        desc.Format = exportDepthFmt;
        hr = m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_SHARED,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_slots[i].depthTex)
        );
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared depth texture for slot %d!", i);
            return false;
        }

        swprintf_s(name, L"Local\\AsyncReproj_Depth_%d", i);
        m_device->CreateSharedHandle(m_slots[i].depthTex, nullptr, GENERIC_ALL, name, &hShared);
        if (hShared) CloseHandle(hShared);

        // Create Shared MV
        desc.Format = (mvFmt != DXGI_FORMAT_UNKNOWN) ? mvFmt : DXGI_FORMAT_R16G16_FLOAT;
        hr = m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_SHARED,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_slots[i].mvTex)
        );
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared MV texture for slot %d!", i);
            return false;
        }

        swprintf_s(name, L"Local\\AsyncReproj_MV_%d", i);
        m_device->CreateSharedHandle(m_slots[i].mvTex, nullptr, GENERIC_ALL, name, &hShared);
        if (hShared) CloseHandle(hShared);

        // Create Shared Fence
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_slots[i].fence));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared fence for slot %d!", i);
            return false;
        }

        swprintf_s(name, L"Local\\AsyncReproj_Fence_%d", i);
        m_device->CreateSharedHandle(m_slots[i].fence, nullptr, GENERIC_ALL, name, &hShared);
        if (hShared) CloseHandle(hShared);

        m_slots[i].fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        m_slots[i].currentFenceValue = 0;
    }

    m_texturesInitialized = true;
    return true;
}

void ExportManager::CopyResourceToExport(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* src, ID3D12Resource* dst) {
    if (!src || !dst) return;

    auto srcDesc = src->GetDesc();
    auto dstDesc = dst->GetDesc();

    if (srcDesc.Format == dstDesc.Format) {
        cmdList->CopyResource(dst, src);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = dst;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = src;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
    }
}

void ExportManager::OnPresent(IDXGISwapChain* swapchain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_device || !m_queue || !m_cmdList) return;

    m_frameId++;

    // 1. Get swapchain backbuffer (Color Source)
    UINT backbufferIdx = 0;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        backbufferIdx = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }

    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(backbufferIdx, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    auto backbufferDesc = backbuffer->GetDesc();
    uint32_t width = (uint32_t)backbufferDesc.Width;
    uint32_t height = backbufferDesc.Height;

    // 2. Get Depth and Motion Vector Candidates
    ResourceTracker::Instance().EndFrame(m_frameId, width, height);
    ID3D12Resource* depthRes = ResourceTracker::Instance().GetBestDepthCandidate();
    ID3D12Resource* mvRes = ResourceTracker::Instance().GetBestMVCandidate();

    DXGI_FORMAT depthFmt = depthRes ? depthRes->GetDesc().Format : DXGI_FORMAT_D32_FLOAT;
    DXGI_FORMAT mvFmt = mvRes ? mvRes->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT;

    // 3. Recreate textures if dimension/format changed
    if (!RecreateExportTextures(width, height, backbufferDesc.Format, depthFmt, mvFmt)) {
        backbuffer->Release();
        return;
    }

    // 4. Select Slot
    uint32_t slotIndex = m_sequenceNumber % 3;
    auto& slot = m_slots[slotIndex];

    // Wait if GPU is still using this slot
    if (slot.fence && slot.currentFenceValue > 0) {
        if (slot.fence->GetCompletedValue() < slot.currentFenceValue) {
            if (slot.fenceEvent) {
                slot.fence->SetEventOnCompletion(slot.currentFenceValue, slot.fenceEvent);
                WaitForSingleObject(slot.fenceEvent, INFINITE);
            } else {
                HANDLE hEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                if (hEvent) {
                    slot.fence->SetEventOnCompletion(slot.currentFenceValue, hEvent);
                    WaitForSingleObject(hEvent, INFINITE);
                    CloseHandle(hEvent);
                }
            }
        }
    }

    // Reset command list
    m_cmdAllocators[slotIndex]->Reset();
    m_cmdList->Reset(m_cmdAllocators[slotIndex], nullptr);

    // Track validity
    uint32_t validityFlags = 0;

    // A. Copy Color (always valid since we use backbuffer)
    {
        D3D12_RESOURCE_STATES originalState = ResourceTracker::Instance().GetResourceState(backbuffer);
        if (originalState == D3D12_RESOURCE_STATE_COMMON) {
            // Swapchain backbuffers are usually in PRESENT state right before present
            originalState = D3D12_RESOURCE_STATE_PRESENT;
        }

        // Transition barriers
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        int barrierCount = 0;

        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = backbuffer;
            barriers[barrierCount].Transition.StateBefore = originalState;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.colorTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);

        CopyResourceToExport(m_cmdList, backbuffer, slot.colorTex);

        // Transition back
        barrierCount = 0;
        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = backbuffer;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.StateAfter = originalState;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.colorTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);
        validityFlags |= 1; // Bit 0: color valid
    }

    // B. Copy Depth (if candidate exists and has valid state & compatible format)
    D3D12_RESOURCE_STATES depthState = depthRes ? ResourceTracker::Instance().GetResourceState(depthRes) : D3D12_RESOURCE_STATE_COMMON;
    if (depthRes && depthState != D3D12_RESOURCE_STATE_COMMON && AreFormatsCompatible(depthRes->GetDesc().Format, slot.depthTex->GetDesc().Format)) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        int barrierCount = 0;

        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = depthRes;
            barriers[barrierCount].Transition.StateBefore = depthState;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.depthTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);

        CopyResourceToExport(m_cmdList, depthRes, slot.depthTex);

        barrierCount = 0;
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = depthRes;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.StateAfter = depthState;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.depthTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);
        validityFlags |= 2; // Bit 1: depth valid
    }

    // C. Copy Motion Vectors (if candidate exists and has valid state & compatible format)
    D3D12_RESOURCE_STATES mvState = mvRes ? ResourceTracker::Instance().GetResourceState(mvRes) : D3D12_RESOURCE_STATE_COMMON;
    if (mvRes && mvState != D3D12_RESOURCE_STATE_COMMON && AreFormatsCompatible(mvRes->GetDesc().Format, slot.mvTex->GetDesc().Format)) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        int barrierCount = 0;

        if (mvState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = mvRes;
            barriers[barrierCount].Transition.StateBefore = mvState;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.mvTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);

        CopyResourceToExport(m_cmdList, mvRes, slot.mvTex);

        barrierCount = 0;
        if (mvState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = mvRes;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.StateAfter = mvState;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrierCount++;
        }

        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = slot.mvTex;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount++;

        m_cmdList->ResourceBarrier(barrierCount, barriers);
        validityFlags |= 4; // Bit 2: mv valid
    }

    m_cmdList->Close();

    // Execute copies
    m_queue->ExecuteCommandLists(1, (ID3D12CommandList**)&m_cmdList);

    // Signal Fence
    slot.currentFenceValue++;
    m_queue->Signal(slot.fence, slot.currentFenceValue);

    // Update shared memory metadata
    if (m_ipcInitialized && m_sharedMetadata) {
        auto& slotMeta = m_sharedMetadata->slots[slotIndex];
        slotMeta.width = m_width;
        slotMeta.height = m_height;
        slotMeta.colorFormat = (uint32_t)m_colorFormat;
        slotMeta.depthFormat = (uint32_t)slot.depthTex->GetDesc().Format;
        slotMeta.mvFormat = (uint32_t)slot.mvTex->GetDesc().Format;
        slotMeta.fenceValue = slot.currentFenceValue;
        slotMeta.validityFlags = validityFlags;
        slotMeta.sequenceNumber = m_sequenceNumber;

        m_sharedMetadata->activeSlotIndex = slotIndex;
        m_sequenceNumber++;
    }

    backbuffer->Release();
}
