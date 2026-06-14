#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>
#include "ipc/shared_metadata.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Vertex structure
struct Vertex {
    float pos[3];
    float uv[2];
};

struct ExportSlot {
    ID3D12Resource* colorTex = nullptr;
    ID3D12Resource* depthTex = nullptr;
    ID3D12Resource* mvTex = nullptr;
    ID3D12Fence* fence = nullptr;
};

// Global variables
HWND g_hWnd = NULL;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain3* g_swapchain = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12CommandAllocator* g_cmdAllocator = nullptr;
ID3D12GraphicsCommandList* g_cmdList = nullptr;
ID3D12RootSignature* g_rootSignature = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12Resource* g_vertexBuffer = nullptr;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView = {};
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = NULL;
uint64_t g_fenceValue = 0;
UINT g_frameIndex = 0;

ID3D12Resource* g_swapchainBuffers[2] = { nullptr, nullptr };

// IPC & Shared resources
HANDLE g_fileMapping = NULL;
SharedRingBuffer* g_sharedMetadata = nullptr;
ExportSlot g_slots[3] = {};
uint32_t g_mode = 0; // 0: Color, 1: Depth, 2: Motion Vectors

// Constants
const UINT FRAME_COUNT = 2;

const char* g_shaderSource = 
"struct VS_INPUT {\n"
"    float3 pos : POSITION;\n"
"    float2 uv : TEXCOORD;\n"
"};\n"
"struct PS_INPUT {\n"
"    float4 pos : SV_POSITION;\n"
"    float2 uv : TEXCOORD;\n"
"};\n"
"PS_INPUT VSMain(VS_INPUT input) {\n"
"    PS_INPUT output;\n"
"    output.pos = float4(input.pos, 1.0f);\n"
"    output.uv = input.uv;\n"
"    return output;\n"
"}\n"
"Texture2D g_texColor : register(t0);\n"
"Texture2D g_texDepth : register(t1);\n"
"Texture2D g_texMV : register(t2);\n"
"SamplerState g_sampler : register(s0);\n"
"cbuffer Params : register(b0) {\n"
"    uint g_mode;\n"
"    uint padding[3];\n"
"};\n"
"float4 PSMain(PS_INPUT input) : SV_Target {\n"
"    if (g_mode == 0) {\n"
"        return g_texColor.Sample(g_sampler, input.uv);\n"
"    } else if (g_mode == 1) {\n"
"        float d = g_texDepth.Sample(g_sampler, input.uv).r;\n"
"        return float4(d, d, d, 1.0f);\n"
"    } else {\n"
"        float2 mv = g_texMV.Sample(g_sampler, input.uv).rg;\n"
"        float r = abs(mv.x) * 10.0f;\n"
"        float g = abs(mv.y) * 10.0f;\n"
"        return float4(r, g, 0.0f, 1.0f);\n"
"    }\n"
"}\n";

void ReleaseSharedResources() {
    for (int i = 0; i < 3; i++) {
        if (g_slots[i].colorTex) { g_slots[i].colorTex->Release(); g_slots[i].colorTex = nullptr; }
        if (g_slots[i].depthTex) { g_slots[i].depthTex->Release(); g_slots[i].depthTex = nullptr; }
        if (g_slots[i].mvTex) { g_slots[i].mvTex->Release(); g_slots[i].mvTex = nullptr; }
        if (g_slots[i].fence) { g_slots[i].fence->Release(); g_slots[i].fence = nullptr; }
    }
}

void TryInitializeSharedResources() {
    if (!g_sharedMetadata) {
        HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\AsyncReproj_SharedMemory");
        if (hMap) {
            g_fileMapping = hMap;
            g_sharedMetadata = (SharedRingBuffer*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRingBuffer));
        }
    }

    if (g_sharedMetadata) {
        // Verify dimensions and formats, reopen if changed
        bool needsReopen = false;
        if (g_slots[0].colorTex) {
            auto desc = g_slots[0].colorTex->GetDesc();
            if (desc.Width != g_sharedMetadata->slots[0].width || desc.Height != g_sharedMetadata->slots[0].height) {
                needsReopen = true;
                printf("Texture dimension change detected. Re-opening handles...\n");
            }
        }

        if (needsReopen) {
            ReleaseSharedResources();
        }

        for (int i = 0; i < 3; i++) {
            if (!g_slots[i].colorTex) {
                HANDLE hShared = nullptr;
                wchar_t name[128];
                swprintf_s(name, L"Local\\AsyncReproj_Color_%d", i);
                if (SUCCEEDED(g_device->OpenSharedHandleByName(name, GENERIC_ALL, &hShared))) {
                    g_device->OpenSharedHandle(hShared, IID_PPV_ARGS(&g_slots[i].colorTex));
                    CloseHandle(hShared);
                }
            }
            if (!g_slots[i].depthTex) {
                HANDLE hShared = nullptr;
                wchar_t name[128];
                swprintf_s(name, L"Local\\AsyncReproj_Depth_%d", i);
                if (SUCCEEDED(g_device->OpenSharedHandleByName(name, GENERIC_ALL, &hShared))) {
                    g_device->OpenSharedHandle(hShared, IID_PPV_ARGS(&g_slots[i].depthTex));
                    CloseHandle(hShared);
                }
            }
            if (!g_slots[i].mvTex) {
                HANDLE hShared = nullptr;
                wchar_t name[128];
                swprintf_s(name, L"Local\\AsyncReproj_MV_%d", i);
                if (SUCCEEDED(g_device->OpenSharedHandleByName(name, GENERIC_ALL, &hShared))) {
                    g_device->OpenSharedHandle(hShared, IID_PPV_ARGS(&g_slots[i].mvTex));
                    CloseHandle(hShared);
                }
            }
            if (!g_slots[i].fence) {
                HANDLE hShared = nullptr;
                wchar_t name[128];
                swprintf_s(name, L"Local\\AsyncReproj_Fence_%d", i);
                if (SUCCEEDED(g_device->OpenSharedHandleByName(name, GENERIC_ALL, &hShared))) {
                    g_device->OpenSharedHandle(hShared, IID_PPV_ARGS(&g_slots[i].fence));
                    CloseHandle(hShared);
                }
            }
        }
    }
}

void WaitForPreviousFrame() {
    const uint64_t fence = g_fenceValue;
    g_queue->Signal(g_fence, fence);
    g_fenceValue++;

    if (g_fence->GetCompletedValue() < fence) {
        g_fence->SetEventOnCompletion(fence, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swapchain->GetCurrentBackBufferIndex();
}

bool InitializeD3D() {
    // 1. Create Device
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    if (FAILED(hr)) return false;

    // 2. Create Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue));
    if (FAILED(hr)) return false;

    // 3. Create Swap Chain
    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = FRAME_COUNT;
    sd.Width = 1280;
    sd.Height = 720;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;

    IDXGISwapChain1* sc = nullptr;
    factory->CreateSwapChainForHwnd(g_queue, g_hWnd, &sd, nullptr, nullptr, &sc);
    sc->QueryInterface(IID_PPV_ARGS(&g_swapchain));
    sc->Release();
    factory->Release();

    g_frameIndex = g_swapchain->GetCurrentBackBufferIndex();

    // 4. Create RTV Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));

    SIZE_T rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    for (UINT i = 0; i < FRAME_COUNT; i++) {
        g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_swapchainBuffers[i]));
        g_device->CreateRenderTargetView(g_swapchainBuffers[i], nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }

    // 5. Create SRV Descriptor Heap (holds Color, Depth, MV textures)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 3; // Color, Depth, MV
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));

    // 6. Create Command Allocator and List
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmdAllocator));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmdAllocator, nullptr, IID_PPV_ARGS(&g_cmdList));
    g_cmdList->Close();

    // 7. Create Root Signature
    D3D12_DESCRIPTOR_RANGE ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = ranges;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Mode parameter as root constant
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[1].Constants.Num32BitValues = 1;
    rootParameters[1].Constants.ShaderRegister = 0;
    rootParameters[1].Constants.RegisterSpace = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParameters;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* signature = nullptr;
    D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr);
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
    signature->Release();

    // 8. Compile Shaders & Create Pipeline State Object (PSO)
    ID3DBlob* vertexShader = nullptr;
    ID3DBlob* pixelShader = nullptr;
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, nullptr);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pixelShader, nullptr);

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = g_rootSignature;
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    vertexShader->Release();
    pixelShader->Release();

    // 9. Create Vertex Buffer (Full-screen quad)
    Vertex vertices[] = {
        { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } }
    };

    D3D12_HEAP_PROPERTIES heapUpload = {};
    heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
    
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(vertices);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    g_device->CreateCommittedResource(&heapUpload, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vertexBuffer));
    
    void* pVertexDataBegin;
    g_vertexBuffer->Map(0, nullptr, &pVertexDataBegin);
    memcpy(pVertexDataBegin, vertices, sizeof(vertices));
    g_vertexBuffer->Unmap(0, nullptr);

    g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();
    g_vertexBufferView.StrideInBytes = sizeof(Vertex);
    g_vertexBufferView.SizeInBytes = sizeof(vertices);

    // 10. Create Presentation Fence
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

void CleanupD3D() {
    WaitForPreviousFrame();
    CloseHandle(g_fenceEvent);
    if (g_fence) g_fence->Release();
    if (g_vertexBuffer) g_vertexBuffer->Release();
    if (g_pso) g_pso->Release();
    if (g_rootSignature) g_rootSignature->Release();
    if (g_cmdList) g_cmdList->Release();
    if (g_cmdAllocator) g_cmdAllocator->Release();
    for (int i = 0; i < FRAME_COUNT; i++) {
        if (g_swapchainBuffers[i]) g_swapchainBuffers[i]->Release();
    }
    if (g_rtvHeap) g_rtvHeap->Release();
    if (g_srvHeap) g_srvHeap->Release();
    if (g_swapchain) g_swapchain->Release();
    if (g_queue) g_queue->Release();
    if (g_device) g_device->Release();
    
    ReleaseSharedResources();
    if (g_sharedMetadata) UnmapViewOfFile(g_sharedMetadata);
    if (g_fileMapping) CloseHandle(g_fileMapping);
}

void Render() {
    TryInitializeSharedResources();

    g_cmdAllocator->Reset();
    g_cmdList->Reset(g_cmdAllocator, g_pso);

    g_cmdList->SetGraphicsRootSignature(g_rootSignature);
    g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);

    int selectedSlot = -1;
    uint64_t highestSeq = 0;

    // Synchronize and select the newest complete slot
    if (g_sharedMetadata) {
        for (int i = 0; i < 3; i++) {
            auto& slotMeta = g_sharedMetadata->slots[i];
            auto& slot = g_slots[i];

            if (slot.fence && slot.colorTex && slot.depthTex && slot.mvTex) {
                // Check if fence completed
                uint64_t completedVal = slot.fence->GetCompletedValue();
                if (completedVal >= slotMeta.fenceValue) {
                    if (slotMeta.sequenceNumber >= highestSeq) {
                        highestSeq = slotMeta.sequenceNumber;
                        selectedSlot = i;
                    }
                }
            }
        }
    }

    // Bind resources of the selected slot
    if (selectedSlot != -1) {
        auto& slot = g_slots[selectedSlot];
        auto& slotMeta = g_sharedMetadata->slots[selectedSlot];

        // Create SRVs in descriptor heap dynamically
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle(g_srvHeap->GetCPUDescriptorHandleForHeapStart());
        SIZE_T srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Bind color
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Format = (DXGI_FORMAT)slotMeta.colorFormat;
        g_device->CreateShaderResourceView(slot.colorTex, &srvDesc, srvHandle);
        srvHandle.ptr += srvDescriptorSize;

        // Bind depth
        srvDesc.Format = (DXGI_FORMAT)slotMeta.depthFormat;
        g_device->CreateShaderResourceView(slot.depthTex, &srvDesc, srvHandle);
        srvHandle.ptr += srvDescriptorSize;

        // Bind MV
        srvDesc.Format = (DXGI_FORMAT)slotMeta.mvFormat;
        g_device->CreateShaderResourceView(slot.mvTex, &srvDesc, srvHandle);

        // Update Window title with status
        char title[256];
        const char* modeStr = (g_mode == 0) ? "Color" : (g_mode == 1 ? "Depth" : "Motion Vectors");
        sprintf_s(title, "AsyncReproj Consumer - Mode: %s | Active Slot: %d | Seq: %llu | Size: %ux%u", 
            modeStr, selectedSlot, slotMeta.sequenceNumber, slotMeta.width, slotMeta.height);
        SetWindowTextA(g_hWnd, title);
    } else {
        SetWindowTextA(g_hWnd, "AsyncReproj Consumer - Waiting for Producer...");
    }

    // Render screen quad
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_swapchainBuffers[g_frameIndex];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    SIZE_T rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtvHandle.ptr += g_frameIndex * rtvDescriptorSize;

    g_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, 1280, 720 };
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissorRect);

    g_cmdList->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    g_cmdList->SetGraphicsRoot32BitConstant(1, g_mode, 0);

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->IASetVertexBuffers(0, 1, &g_vertexBufferView);
    g_cmdList->DrawInstanced(6, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdList->ResourceBarrier(1, &barrier);

    g_cmdList->Close();
    g_queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g_cmdList);

    g_swapchain->Present(1, 0);
    WaitForPreviousFrame();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_KEYDOWN:
            if (wParam == '1') g_mode = 0; // Color
            if (wParam == '2') g_mode = 1; // Depth
            if (wParam == '3') g_mode = 2; // MV
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int main() {
    WNDCLASSEXA wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXA);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = GetModuleHandle(nullptr);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = "ConsumerClass";
    RegisterClassExA(&wcex);

    g_hWnd = CreateWindowExA(0, "ConsumerClass", "AsyncReproj Consumer - Connecting...", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, wcex.hInstance, nullptr);

    if (!g_hWnd) return -1;

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    if (!InitializeD3D()) {
        MessageBoxA(nullptr, "Failed to initialize D3D12!", "Error", MB_OK);
        return -1;
    }

    printf("AsyncReproj Consumer started. Controls:\n  [1] - View Color\n  [2] - View Depth\n  [3] - View Motion Vectors\n");

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            Render();
        }
    }

    CleanupD3D();
    return (int)msg.wParam;
}
