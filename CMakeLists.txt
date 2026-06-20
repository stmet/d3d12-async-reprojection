cmake_minimum_required(VERSION 3.20)
project(AsyncReprojection C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored Dear ImGui (1.92, FreeType disabled — embedded stb_truetype). Core + DX12/Win32 backends.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
)

# Define the proxy DLL target
add_library(dxgi SHARED
    src/common/logger.h
    src/common/logger.cpp
    src/proxy/dxgi_proxy.h
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/hooks/hook_manager.h
    src/hooks/hook_manager.cpp
    src/overlay/overlay.h
    src/overlay/overlay.cpp
    src/swapchain/proxy_swapchain.h
    src/swapchain/proxy_swapchain.cpp
    src/input/mouse_tracker.h
    src/input/mouse_tracker.cpp
    src/warp/warp_renderer.h
    src/warp/warp_renderer.cpp
    src/present/presenter.h
    src/present/presenter.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(dxgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/detours
    ${IMGUI_DIR}
)

# NOTE: deliberately NOT linking dxgi.lib — this DLL is named dxgi.dll and resolves the DXGI
# entry points through its own exported forwarders (src/proxy/dxgi_proxy.cpp). d3d12.lib is
# required by imgui_impl_dx12.cpp (D3D12SerializeRootSignature); d3dcompiler/gdi32/dwmapi/imm32
# are auto-linked via #pragma comment(lib, ...) inside the ImGui backends.
target_link_libraries(dxgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/detours/detours.lib
    d3d12
)
