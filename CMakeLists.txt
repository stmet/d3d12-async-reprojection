cmake_minimum_required(VERSION 3.20)
project(AsyncReprojection C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the proxy DLL target
add_library(dxgi SHARED
    src/common/logger.h
    src/common/logger.cpp
    src/proxy/dxgi_proxy.h
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/hooks/hook_manager.h
    src/hooks/hook_manager.cpp
    src/tracker/resource_tracker.h
    src/tracker/resource_tracker.cpp
    src/export/export_manager.h
    src/export/export_manager.cpp
    src/ipc/shared_metadata.h
)

target_include_directories(dxgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/detours
)

target_link_libraries(dxgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/detours/detours.lib
)

# Include consumer subdirectory
add_subdirectory(consumer)
