cmake_minimum_required(VERSION 3.21)

option(SCANENGINE_ENABLE_MLX_CUDA
    "Link the vendored Windows MLX 0.31.1 CUDA static library" OFF)
if(SCANENGINE_ENABLE_MLX_CUDA AND CMAKE_VERSION VERSION_LESS "3.25")
    message(FATAL_ERROR
        "SCANENGINE_ENABLE_MLX_CUDA requires CMake >= 3.25; actual CMake is "
        "${CMAKE_VERSION}")
endif()

# ONNX Runtime 1.28.0 official arm64 SDK/wheel both carry LC_BUILD_VERSION
# minos=14.0. Lock this before project() so compiler/linker deployment flags
# agree with the runtime instead of producing a package that fails in dyld.
if(CMAKE_HOST_APPLE)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
       CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
            "Minimum macOS required by locked ONNX Runtime 1.28.0" FORCE)
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0")
        message(FATAL_ERROR
            "ONNX Runtime 1.28.0 arm64 requires CMAKE_OSX_DEPLOYMENT_TARGET >= 14.0")
    endif()
endif()
project(ScanEngine VERSION 0.1.0 LANGUAGES C CXX)

if(SCANENGINE_ENABLE_MLX_CUDA AND NOT WIN32)
    message(FATAL_ERROR
        "SCANENGINE_ENABLE_MLX_CUDA is supported only by the unified Windows "
        "MSVC x64 product build")
endif()

if(WIN32 AND NOT SCANENGINE_ENABLE_MLX_CUDA)
    message(FATAL_ERROR
        "The Windows product is GPU-only. Configure with the "
        "win-qt5.12.9-msvc-mlx-cuda preset.")
endif()

if(APPLE)
    include(cmake/MacOS.cmake)
elseif(WIN32)
    include(cmake/Windows.cmake)
else()
    message(FATAL_ERROR "ScanEngine supports macOS and Windows")
endif()
