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
    for (int i = 0; i < 3; i++) {
        if (m_slots[i].colorTex) { m_slots[i].colorTex->Release(); m_slots[i].colorTex = nullptr; }
        if (m_slots[i].depthTex) { m_slots[i].depthTex->Release(); m_slots[i].depthTex = nullptr; }
        if (m_slots[i].mvTex) { m_slots[i].mvTex->Release(); m_slots[i].mvTex = nullptr; }
        if (m_slots[i].fence) { m_slots[i].fence->Release(); m_slots[i].fence = nullptr; }
        m_slots[i].currentFenceValue = 0;
    }
    m_texturesInitialized = false;
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

        // Create Shared Depth (expose as R32_FLOAT)
        DXGI_FORMAT exportDepthFmt = DXGI_FORMAT_R32_FLOAT;
        if (depthFmt == DXGI_FORMAT_D16_UNORM || depthFmt == DXGI_FORMAT_R16_TYPELESS) {
            exportDepthFmt = DXGI_FORMAT_R16_UNORM;
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
    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
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

    // B. Copy Depth (if candidate exists)
    if (depthRes) {
        D3D12_RESOURCE_STATES originalState = ResourceTracker::Instance().GetResourceState(depthRes);
        if (originalState == D3D12_RESOURCE_STATE_COMMON) {
            originalState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        int barrierCount = 0;

        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = depthRes;
            barriers[barrierCount].Transition.StateBefore = originalState;
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
        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = depthRes;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.StateAfter = originalState;
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

    // C. Copy Motion Vectors (if candidate exists)
    if (mvRes) {
        D3D12_RESOURCE_STATES originalState = ResourceTracker::Instance().GetResourceState(mvRes);
        if (originalState == D3D12_RESOURCE_STATE_COMMON) {
            originalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        int barrierCount = 0;

        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = mvRes;
            barriers[barrierCount].Transition.StateBefore = originalState;
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
        if (originalState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Transition.pResource = mvRes;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[barrierCount].Transition.StateAfter = originalState;
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
        slotMeta.depthFormat = (uint32_t)depthFmt;
        slotMeta.mvFormat = (uint32_t)mvFmt;
        slotMeta.fenceValue = slot.currentFenceValue;
        slotMeta.validityFlags = validityFlags;
        slotMeta.sequenceNumber = m_sequenceNumber;

        m_sharedMetadata->activeSlotIndex = slotIndex;
        m_sequenceNumber++;
    }

    backbuffer->Release();
}
