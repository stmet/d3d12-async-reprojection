cmake_minimum_required(VERSION 3.20)

add_executable(ConsumerApp
    main.cpp
    ../src/ipc/shared_metadata.h
)

target_include_directories(ConsumerApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(ConsumerApp PRIVATE
    d3d12
    dxgi
    d3dcompiler
)
